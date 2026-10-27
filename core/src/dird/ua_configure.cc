/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2015-2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
// Written by Marco van Wieringen, January 2015
/**
 * @file
 * Interactive configuration engine for director.
 */

#include "include/fcntl_def.h"
#include "include/bareos.h"
#include "dird.h"
#include "dird/dird_globals.h"
#include "dird/ua_select.h"
#include "dird/configure_edit.h"
#include "dird/dird_conf.h"
#include "dird/reload.h"
#include "lib/parse_conf.h"
#include "lib/util.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <optional>
#include <map>
#include <set>
#include <vector>
#include <thread>
#include <sys/stat.h>

namespace directordaemon {

static void ConfigureLexErrorHandler(const char*, int, lexer* lc, PoolMem& msg)
{
  UaContext* ua;

  lc->error_counter++;
  if (lc->caller_ctx) {
    ua = (UaContext*)(lc->caller_ctx);
    ua->ErrorMsg("configure error: %s\n", msg.c_str());
  }
}

static void ConfigureLexErrorHandler(const char* file,
                                     int line,
                                     lexer* lc,
                                     const char* msg,
                                     ...)
{
  // This function is an error handler, used by lex.
  PoolMem buf(PM_NAME);
  va_list ap;

  va_start(ap, msg);
  buf.Bvsprintf(msg, ap);
  va_end(ap);

  ConfigureLexErrorHandler(file, line, lc, buf);
}

static inline bool configure_write_resource(const char* filename,
                                            const char*,
                                            const char*,
                                            const char* content,
                                            const bool overwrite = false)
{
  bool result = false;
  size_t len;
  int fd;
  int flags = O_CREAT | O_WRONLY | O_TRUNC;

  if (!overwrite) { flags |= O_EXCL; }

  if ((fd = open(filename, flags, 0640)) >= 0) {
    len = strlen(content);
    if (write(fd, content, len) < 0) {
      result = false;
    } else {
      result = true;
    }
    close(fd);
  }

  return result;
}

static inline const ResourceItem* config_get_res_item(
    UaContext* ua,
    const ResourceTable* res_table,
    const char* key,
    const char* value)
{
  const ResourceItem* item = NULL;
  const char* errorcharmsg = NULL;

  if (res_table) {
    item = my_config->GetResourceItem(res_table->items, key);
    if (!item) {
      ua->ErrorMsg("Resource \"%s\" does not permit directive \"%s\".\n",
                   res_table->name, key);
      return NULL;
    }
  }

  /* Check against dangerous characters ('@', ';').
   * Could be less strict, if this characters are quoted,
   * but it is easier to handle it like this. */
  if (strchr(value, '@')) { errorcharmsg = "'@' (include)"; }
  if (strchr(value, ';')) { errorcharmsg = "';' (end of directive)"; }
  if (errorcharmsg) {
    if (ua) {
      ua->ErrorMsg(
          "Could not add directive \"%s\": character %s is forbidden.\n", key,
          errorcharmsg);
    }
    return NULL;
  }

  return item;
}

static inline bool config_add_directive(UaContext* ua,
                                        const ResourceTable* res_table,
                                        const char* key,
                                        const char* value,
                                        PoolMem& resource,
                                        int indent = 2)
{
  PoolMem temp(PM_MESSAGE);
  const ResourceItem* item = NULL;
  std::string format("%-*s%s = %s\n");

  const std::array<int, 15> quotable_types{
      CFG_TYPE_AUTOPASSWORD,   CFG_TYPE_CLEARPASSWORD, CFG_TYPE_DIR,
      CFG_TYPE_DIR_OR_CMD,     CFG_TYPE_LABEL,         CFG_TYPE_MD5PASSWORD,
      CFG_TYPE_NAME,           CFG_TYPE_RES,           CFG_TYPE_RUNSCRIPT_CMD,
      CFG_TYPE_RUNSCRIPT_WHEN, CFG_TYPE_SHRTRUNSCRIPT, CFG_TYPE_STDSTR,
      CFG_TYPE_STDSTRDIR,      CFG_TYPE_STR,           CFG_TYPE_STRNAME};

  const std::array<int, 3> script_types{
      CFG_TYPE_RUNSCRIPT_CMD, CFG_TYPE_DIR_OR_CMD, CFG_TYPE_SHRTRUNSCRIPT};

  std::string temp_value(value);

  item = config_get_res_item(ua, res_table, key, value);

  if (res_table && (!item)) { return false; }

  if (item) {
    // Use item->name instead of key for uniform formatting.
    key = item->name;
    if (std::find(quotable_types.begin(), quotable_types.end(), item->type)
        != quotable_types.end()) {
      format = "%-*s%s = \"%s\"\n";

      if (std::find(script_types.begin(), script_types.end(), item->type)
          != script_types.end()) {
        temp_value = EscapeString(value);
      }
    }
  }

  temp.bsprintf(format.c_str(), indent, "", key, temp_value.c_str());

  resource.strcat(temp);

  return true;
}

static inline bool configure_create_resource_string(
    UaContext* ua,
    int first_parameter,
    const ResourceTable* res_table,
    PoolMem& resourcename,
    PoolMem& resource)
{
  resource.strcat(res_table->name);
  resource.strcat(" {\n");

  /* Is the name of the resource already given as value of the resource type?
   * E.g. configure add client=newclient address=127.0.0.1 ...
   * instead of configure add client name=newclient address=127.0.0.1 ... */
  if (ua->argv[first_parameter - 1]) {
    resourcename.strcat(ua->argv[first_parameter - 1]);
    if (!config_add_directive(ua, res_table, "name", resourcename.c_str(),
                              resource)) {
      return false;
    }
  }

  for (int i = first_parameter; i < ua->argc; i++) {
    if (!ua->argv[i]) {
      ua->ErrorMsg("Missing value for directive \"%s\"\n", ua->argk[i]);
      return false;
    }
    if (Bstrcasecmp(ua->argk[i], "name")) { resourcename.strcat(ua->argv[i]); }
    if (!config_add_directive(ua, res_table, ua->argk[i], ua->argv[i],
                              resource)) {
      return false;
    }
  }
  resource.strcat("}\n");

  if (strlen(resourcename.c_str()) <= 0) {
    ua->ErrorMsg("Resource \"%s\": missing name parameter.\n", res_table->name);
    return false;
  }

  return true;
}

static inline bool ConfigureCreateFdResourceString(UaContext* ua,
                                                   PoolMem& resource,
                                                   const char* clientname)
{
  ClientResource* client;
  s_password* password;
  PoolMem temp(PM_MESSAGE);

  client = ua->GetClientResWithName(clientname);
  if (!client) { return false; }
  password = &client->password_;

  resource.strcat("Director {\n");

  Mmsg(temp, "\"%s\"", me->resource_name_);
  config_add_directive(NULL, NULL, "Name", temp.c_str(), resource);

  switch (password->encoding) {
    case p_encoding_clear:
      Mmsg(temp, "\"%s\"", password->value);
      break;
    case p_encoding_md5:
      Mmsg(temp, "\"[md5]%s\"", password->value);
      break;
    default:
      break;
  }
  config_add_directive(NULL, NULL, "Password", temp.c_str(), resource);

  resource.strcat("}\n");

  return true;
}

/**
 * Create a bareos-fd director resource file
 * that corresponds to our client definition.
 */
static inline bool ConfigureCreateFdResource(UaContext* ua,
                                             const char* clientname)
{
  PoolMem resource(PM_MESSAGE);
  PoolMem filename_tmp(PM_FNAME);
  PoolMem filename(PM_FNAME);
  PoolMem basedir(PM_FNAME);
  PoolMem temp(PM_MESSAGE);
  const char* dirname = NULL;
  const bool error_if_exists = false;
  const bool create_directories = true;
  const bool overwrite = true;

  if (!ConfigureCreateFdResourceString(ua, resource, clientname)) {
    return false;
  }

  // Get the path where the resource should get stored.
  basedir.bsprintf("bareos-dir-export/client/%s/bareos-fd.d", clientname);
  dirname = my_config->GetNextRes(R_DIRECTOR, NULL)->resource_name_;
  if (!my_config->GetPathOfNewResource(filename, temp, basedir.c_str(),
                                       "director", dirname, error_if_exists,
                                       create_directories)) {
    ua->ErrorMsg("%s", temp.c_str());
    return false;
  }
  filename_tmp.strcpy(temp);

  // Write resource to file.
  if (!configure_write_resource(filename.c_str(), "filedaemon-export",
                                clientname, resource.c_str(), overwrite)) {
    ua->ErrorMsg("failed to write filedaemon config resource file\n");
    return false;
  }

  ua->send->ObjectStart("export");
  ua->send->ObjectKeyValue("clientname", clientname);
  ua->send->ObjectKeyValue("component", "bareos-fd");
  ua->send->ObjectKeyValue("resource", "director");
  ua->send->ObjectKeyValue("name", dirname);
  ua->send->ObjectKeyValue("filename", filename.c_str(),
                           "Exported resource file \"%s\":\n");
  ua->send->ObjectKeyValue("content", resource.c_str(), "%s");
  ua->send->ObjectEnd("export");

  return true;
}

/**
 * To add a resource during runtime, the following approach is used:
 *
 * - Create a temporary file which contains the new resource.
 * - Use the existing parsing functions to add the new resource to the
 * configured resources.
 *   - on error:
 *     - remove the resource and the temporary file.
 *   - on success:
 *     - move the new temporary resource file to a place, where it will also be
 * loaded on restart
 *       (<CONFIGDIR>/bareos-dir.d/<resourcetype>/<name_of_the_resource>.conf).
 *
 * This way, the existing parsing functionality is used.
 */
static inline bool ConfigureAddResource(UaContext* ua,
                                        int first_parameter,
                                        const ResourceTable* res_table)
{
  PoolMem resource(PM_MESSAGE);
  PoolMem name(PM_MESSAGE);
  PoolMem filename_tmp(PM_FNAME);
  PoolMem filename(PM_FNAME);
  PoolMem temp(PM_FNAME);
  JobResource* res = NULL;

  if (!configure_create_resource_string(ua, first_parameter, res_table, name,
                                        resource)) {
    return false;
  }

  if (my_config->GetResWithName(res_table->rcode, name.c_str())) {
    ua->ErrorMsg("Resource \"%s\" with name \"%s\" already exists.\n",
                 res_table->name, name.c_str());
    return false;
  }

  if (!my_config->GetPathOfNewResource(filename, temp, NULL, res_table->name,
                                       name.c_str(), true)) {
    ua->ErrorMsg("%s", temp.c_str());
    return false;
  } else {
    filename_tmp.strcpy(temp);
  }

  if (!configure_write_resource(filename_tmp.c_str(), res_table->name,
                                name.c_str(), resource.c_str())) {
    ua->ErrorMsg("failed to write config resource file\n");
    return false;
  }

  my_config->err_type_ = M_ERROR;
  if (!my_config->ParseConfigFile(filename_tmp.c_str(), ua,
                                  ConfigureLexErrorHandler, NULL)) {
    unlink(filename_tmp.c_str());
    my_config->RemoveResource(res_table->rcode, name.c_str());
    return false;
  }

  /* ParseConfigFile has already done some validation.
   * However, it skipped at least some checks for R_JOB
   * (reason: a job can get values from jobdefs,
   * and the value propagation happens after reading the full configuration)
   * therefore we explicitly check the new resource here. */
  if ((res_table->rcode == R_JOB) || (res_table->rcode == R_JOBDEFS)) {
    res = (JobResource*)my_config->GetResWithName(res_table->rcode,
                                                  name.c_str());
    PropagateJobdefs(res_table->rcode, res);
    if (!ValidateResource(res_table->rcode, res_table->items,
                          (BareosResource*)res)) {
      ua->ErrorMsg("failed to create config resource \"%s\"\n", name.c_str());
      unlink(filename_tmp.c_str());
      my_config->RemoveResource(res_table->rcode, name.c_str());
      return false;
    }
  }

  // new config resource is working fine. Rename file to its permanent name.
  if (rename(filename_tmp.c_str(), filename.c_str()) != 0) {
    ua->ErrorMsg("failed to create config file \"%s\"\n", filename.c_str());
    unlink(filename_tmp.c_str());
    my_config->RemoveResource(res_table->rcode, name.c_str());
    return false;
  }

  // When adding a client, also create the File Daemon client resource file.
  if (res_table->rcode == R_CLIENT) {
    ConfigureCreateFdResource(ua, name.c_str());
  }

  ua->send->ObjectStart("add");
  ua->send->ObjectKeyValue("resource", res_table->name);
  ua->send->ObjectKeyValue("name", name.c_str());
  ua->send->ObjectKeyValue("filename", filename.c_str(),
                           "Created resource config file \"%s\":\n");
  ua->send->ObjectKeyValue("content", resource.c_str(), "%s");
  ua->send->ObjectEnd("add");

  return true;
}

static inline bool ConfigureAdd(UaContext* ua, int resource_type_parameter)
{
  bool result = false;
  const ResourceTable* res_table
      = my_config->GetResourceTable(ua->argk[resource_type_parameter]);
  if (!res_table) {
    ua->ErrorMsg(T_("invalid resource type %s.\n"),
                 ua->argk[resource_type_parameter]);
    return false;
  }

  if (res_table->rcode == R_DIRECTOR) {
    ua->ErrorMsg(T_("Only one Director resource allowed.\n"));
    return false;
  }

  ua->send->ObjectStart("configure");
  result = ConfigureAddResource(ua, resource_type_parameter + 1, res_table);
  ua->send->ObjectEnd("configure");

  return result;
}

static inline void ConfigureExportUsage(UaContext* ua)
{
  ua->ErrorMsg(T_("usage: configure export client=<clientname>\n"));
}

static inline bool ConfigureExport(UaContext* ua)
{
  bool result = false;
  int i;

  i = FindArgWithValue(ua, NT_("client"));
  if (i < 0) {
    ConfigureExportUsage(ua);
    return false;
  }

  if (!ua->GetClientResWithName(ua->argv[i])) {
    ConfigureExportUsage(ua);
    return false;
  }

  ua->send->ObjectStart("configure");
  result = ConfigureCreateFdResource(ua, ua->argv[i]);
  ua->send->ObjectEnd("configure");

  return result;
}

// How long a configure change waits for another change or reload to end.
static int config_change_wait_ms = 30000;

// Holds the configuration change token for the scope of a command.
class ConfigChangeGuard {
 public:
  ConfigChangeGuard()
  {
    for (int waited = 0;; waited += 100) {
      auto t = TryBeginConfigChange();
      if (t) {
        token_.emplace(std::move(*t));
        return;
      }
      if (waited >= config_change_wait_ms) { return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  bool held() const { return token_.has_value(); }
  const ConfigChangeToken& token() const { return *token_; }

 private:
  std::optional<ConfigChangeToken> token_;
};

// [A-Za-z0-9][A-Za-z0-9._-]* without ".."
static bool ValidResourceName(const char* name)
{
  if (!name || !isalnum((unsigned char)name[0]) || strstr(name, "..")) {
    return false;
  }
  for (const char* p = name; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
      return false;
    }
  }
  return true;
}

// Non-empty and free of characters that could change the file's structure.
static bool ValueIsSafe(const char* v)
{
  return v && *v && !strpbrk(v, "@;\"\\\r\n");
}

// Removes a trailing or inner "preview" argument; true if it was there.
static bool ExtractPreview(UaContext* ua)
{
  for (int i = 1; i < ua->argc; i++) {
    if (Bstrcasecmp(ua->argk[i], "preview") && !ua->argv[i]) {
      for (int j = i; j + 1 < ua->argc; j++) {
        ua->argk[j] = ua->argk[j + 1];
        ua->argv[j] = ua->argv[j + 1];
      }
      ua->argc--;
      return true;
    }
  }
  return false;
}

static std::string DirName(const std::string& p)
{
  size_t s = p.rfind('/');
  if (s == std::string::npos) { return "."; }
  return s == 0 ? "/" : p.substr(0, s);
}

static std::string RealPath(const std::string& p)
{
  char buf[PATH_MAX];
  if (!realpath(p.c_str(), buf)) { return std::string(); }
  return buf;
}

static std::string BaseName(const std::string& p)
{
  return p.substr(p.rfind('/') + 1);
}

static bool IsRealDirectory(const std::string& p)
{
  struct stat st;
  return lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// File of a resource as <realpath(configdir)>/<include dir>/<type>/<name>.conf;
// the include and type directories must be real directories, not symlinks.
static bool ManagedFilePath(UaContext* ua,
                            const ResourceTable* res_table,
                            const char* name,
                            std::string& path)
{
  PoolMem p(PM_FNAME);

  if (!my_config->IsUsingConfigIncludeDir()
      || !my_config->GetPathOfResource(p, nullptr, res_table->name, name,
                                       false)) {
    ua->ErrorMsg(
        T_("The configuration does not use the include directory layout.\n"));
    return false;
  }
  const std::string canon = p.c_str();
  const std::string type_path = DirName(canon);
  const std::string include_path = DirName(type_path);
  const std::string root = RealPath(DirName(include_path));
  if (root.empty() || BaseName(canon) != std::string(name) + ".conf") {
    ua->ErrorMsg(T_("The configuration directory layout is not supported.\n"));
    return false;
  }

  const std::string include_dir = root + "/" + BaseName(include_path);
  const std::string type_dir = include_dir + "/" + BaseName(type_path);
  if (!IsRealDirectory(include_dir) || !IsRealDirectory(type_dir)) {
    ua->ErrorMsg(T_("\"%s\" is not a real directory inside the configuration "
                    "tree.\n"),
                 type_dir.c_str());
    return false;
  }
  path = type_dir + "/" + name + ".conf";
  return true;
}

static bool ReadWholeFile(const std::string& path, std::string& data)
{
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) { return false; }
  struct stat st;
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
  char buf[4096];
  data.clear();
  while (ok) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      ok = false;
    } else if (n == 0) {
      break;
    } else {
      data.append(buf, n);
    }
  }
  close(fd);
  return ok;
}

// Writes <path>.tmp (new file, no symlink), fsync; returns its name. An
// existing .tmp makes this fail and is never touched.
static bool WriteTmp(const std::string& path,
                     const std::string& data,
                     std::string& tmp,
                     std::string& err)
{
  tmp = path + ".tmp";
  int fd = open(tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0640);
  if (fd < 0) {
    err = "cannot create " + tmp + ": " + strerror(errno);
    return false;
  }
  bool ok = true;
  size_t done = 0;
  while (ok && done < data.size()) {
    ssize_t n = write(fd, data.data() + done, data.size() - done);
    if (n <= 0) {
      ok = false;
    } else {
      done += n;
    }
  }
  if (ok && fsync(fd) != 0) { ok = false; }
  if (close(fd) != 0) { ok = false; }
  if (!ok) {
    err = "cannot write " + tmp + ": " + strerror(errno);
    unlink(tmp.c_str());
  }
  return ok;
}

// Test hook, null in production: returns true to make a ReplaceFile fail.
bool (*ConfigureWriteFailHook)(const std::string& path,
                               const std::string& data)
    = nullptr;

// Replaces path by data (tmp + rename).
static bool ReplaceFile(const std::string& path,
                        const std::string& data,
                        std::string& err)
{
  if (ConfigureWriteFailHook && ConfigureWriteFailHook(path, data)) {
    err = "write failed (test hook)";
    return false;
  }
  std::string tmp;
  if (!WriteTmp(path, data, tmp, err)) { return false; }
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    err = "cannot rename " + tmp + ": " + strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

struct PublishOutcome {
  bool published{false};
  bool reloaded{false};
  const char* rollback{"none"};
};

// Publishes updated over the file, reloads; a failed reload puts orig back.
static PublishOutcome PublishSet(const ConfigChangeToken& token,
                                 const std::string& path,
                                 const std::string& orig,
                                 const std::string& updated,
                                 std::string& err)
{
  PublishOutcome o;
  if (!ReplaceFile(path, updated, err)) { return o; }
  o.published = true;
  o.reloaded = ReloadConfigHeld(token);
  if (!o.reloaded) {
    std::string e, back;
    bool ok = ReplaceFile(path, orig, e) && ReadWholeFile(path, back)
              && back == orig;
    o.rollback = ok ? "restored" : "failed";
  }
  return o;
}

static void ConfigureSendOutcome(UaContext* ua,
                                 const PublishOutcome& o,
                                 bool preview)
{
  ua->send->ObjectKeyValueBool("preview", preview, "Preview: %s\n");
  ua->send->ObjectKeyValueBool("reloaded", o.reloaded, "Reloaded: %s\n");
  ua->send->ObjectKeyValue("rollback", o.rollback, "Rollback: %s\n");
}

static bool ConfigureReportFailure(UaContext* ua,
                                   const PublishOutcome& o,
                                   const std::string& path)
{
  if (!o.reloaded && o.published) {
    if (!strcmp(o.rollback, "failed")) {
      ua->ErrorMsg(T_("ROLLBACK FAILED: reload failed, check \"%s\"\n"),
                   path.c_str());
    } else {
      ua->ErrorMsg(T_("reload failed, the file was %s\n"), o.rollback);
    }
  }
  return o.reloaded;
}

// Checks that every comma-separated name is an existing, permitted Storage.
static bool CheckStorageList(UaContext* ua, const char* list)
{
  std::string s(list);
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t e = s.find(',', pos);
    if (e == std::string::npos) { e = s.size(); }
    std::string n = s.substr(pos, e - pos);
    size_t b = n.find_first_not_of(" \t");
    size_t f = n.find_last_not_of(" \t");
    n = b == std::string::npos ? std::string() : n.substr(b, f - b + 1);
    if (!ValidResourceName(n.c_str())) {
      ua->ErrorMsg(T_("Invalid storage name \"%s\" in the list.\n"), n.c_str());
      return false;
    }
    if (!ua->GetStoreResWithName(n.c_str())) {
      ua->ErrorMsg(T_("Storage \"%s\" does not exist or is not permitted.\n"),
                   n.c_str());
      return false;
    }
    pos = e + 1;
  }
  return true;
}

static void ConfigureSetUsage(UaContext* ua)
{
  ua->ErrorMsg(
      T_("usage:\n"
         "  configure set job=<n>|jobdefs=<n>|pool=<n> "
         "storagegrouppolicy=<ListedOrder|LeastUsed> [preview]\n"
         "  configure set job=<n>|jobdefs=<n>|pool=<n> storage=<A,B,...> "
         "[preview]\n"
         "  configure set storage=<n> enabled=<yes|no> [preview]\n"));
}

static bool ConfigureSetLocked(UaContext* ua,
                               const ConfigChangeToken& token,
                               bool preview)
{
  const char* target_key = ua->argk[2];
  const char* name = ua->argv[2];
  const char* directive = ua->argk[3];
  const char* value = ua->argv[3];
  const char* table_name = nullptr;
  int rcode = 0;

  if (Bstrcasecmp(target_key, "job")) {
    table_name = "Job";
    rcode = R_JOB;
  } else if (Bstrcasecmp(target_key, "jobdefs")) {
    table_name = "JobDefs";
    rcode = R_JOBDEFS;
  } else if (Bstrcasecmp(target_key, "pool")) {
    table_name = "Pool";
    rcode = R_POOL;
  } else if (Bstrcasecmp(target_key, "storage")) {
    table_name = "Storage";
    rcode = R_STORAGE;
  } else {
    ConfigureSetUsage(ua);
    return false;
  }

  const bool is_storage = rcode == R_STORAGE;
  const char* wanted = is_storage ? "enabled" : nullptr;
  if (is_storage ? !Bstrcasecmp(directive, wanted)
                 : !(Bstrcasecmp(directive, "storagegrouppolicy")
                     || Bstrcasecmp(directive, "storage"))) {
    ConfigureSetUsage(ua);
    return false;
  }
  if (!name || !value || !*value) {
    ua->ErrorMsg(T_("A name and a non-empty value are required.\n"));
    return false;
  }
  if (!ValidResourceName(name)) {
    ua->ErrorMsg(T_("Invalid resource name \"%s\".\n"), name);
    return false;
  }
  if (!ValueIsSafe(value)) {
    ua->ErrorMsg(T_("The value contains a forbidden character.\n"));
    return false;
  }

  const ResourceTable* res_table = my_config->GetResourceTable(table_name);
  const ResourceItem* item
      = res_table ? my_config->GetResourceItem(res_table->items, directive)
                  : nullptr;
  if (!item) {
    ua->ErrorMsg(T_("Resource \"%s\" does not permit directive \"%s\".\n"),
                 table_name, directive);
    return false;
  }

  if (!ua->GetResWithName(rcode, name)) {
    ua->ErrorMsg(T_("%s \"%s\" does not exist or is not permitted.\n"),
                 res_table->name, name);
    return false;
  }

  std::string new_value;
  if (Bstrcasecmp(directive, "storagegrouppolicy")) {
    for (int i = 0; storage_group_policies[i]; i++) {
      if (Bstrcasecmp(value, storage_group_policies[i])) {
        new_value = storage_group_policies[i];
      }
    }
    if (new_value.empty()) {
      ua->ErrorMsg(T_("Invalid policy \"%s\", use ListedOrder or LeastUsed.\n"),
                   value);
      return false;
    }
  } else if (Bstrcasecmp(directive, "storage")) {
    if (!CheckStorageList(ua, value)) { return false; }
    new_value = value;
  } else {
    if (Bstrcasecmp(value, "yes") || Bstrcasecmp(value, "true")) {
      new_value = "yes";
    } else if (Bstrcasecmp(value, "no") || Bstrcasecmp(value, "false")) {
      new_value = "no";
    } else {
      ua->ErrorMsg(T_("Invalid value \"%s\", use yes or no.\n"), value);
      return false;
    }
  }

  std::string path, orig, updated, old_value, err;
  struct stat st;
  if (!ManagedFilePath(ua, res_table, name, path)) { return false; }
  if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)
      || !ReadWholeFile(path, orig)) {
    ua->ErrorMsg(T_("\"%s\" is not a managed file, edit by hand.\n"),
                 path.c_str());
    return false;
  }
  if (!CheckManagedResourceText(orig, res_table->name, name, err)
      || !SetDirective(orig, item->name, new_value, updated, old_value, err)) {
    ua->ErrorMsg(T_("\"%s\": %s\n"), path.c_str(), err.c_str());
    return false;
  }

  PublishOutcome outcome;
  outcome.rollback = "none";
  if (!preview) {
    outcome = PublishSet(token, path, orig, updated, err);
    if (!outcome.published) {
      ua->ErrorMsg(T_("%s\n"), err.c_str());
      return false;
    }
  }

  ua->send->ObjectStart("set");
  ua->send->ObjectKeyValue("resource", res_table->name, "Resource: %s\n");
  ua->send->ObjectKeyValue("name", name, "Name: %s\n");
  ua->send->ObjectKeyValue("directive", item->name, "Directive: %s\n");
  ua->send->ObjectKeyValue("old", old_value.c_str(), "Old: %s\n");
  ua->send->ObjectKeyValue("new", new_value.c_str(), "New: %s\n");
  ua->send->ObjectKeyValue("filename", path.c_str(), "File: %s\n");
  ConfigureSendOutcome(ua, outcome, preview);
  ua->send->ObjectEnd("set");

  return preview ? true : ConfigureReportFailure(ua, outcome, path);
}

static bool ConfigureSet(UaContext* ua)
{
  bool preview = ExtractPreview(ua);
  if (ua->argc != 4) {
    ConfigureSetUsage(ua);
    return false;
  }

  ConfigChangeGuard guard;
  if (!guard.held()) {
    ua->ErrorMsg(T_("Director busy, try again.\n"));
    return false;
  }

  ua->send->ObjectStart("configure");
  bool result = ConfigureSetLocked(ua, guard.token(), preview);
  ua->send->ObjectEnd("configure");
  return result;
}

bool ConfigureCmd(UaContext* ua, const char*)
{
  bool result = false;

  if (!(my_config->IsUsingConfigIncludeDir())) {
    ua->WarningMsg(
        T_("It seems that the configuration is not adapted to the include "
           "directory structure. "
           "This means, that the configure command may not work as expected. "
           "Your configuration changes may not survive a reload/restart. "));
  }

  if (ua->argc < 3) {
    ua->ErrorMsg(
        T_("usage:\n"
           "  configure add <resourcetype> <key1>=<value1> ...\n"
           "  configure export client=<clientname>\n"
           "  configure set job=<n>|jobdefs=<n>|pool=<n> "
           "storagegrouppolicy=<v>|storage=<A,B,...> [preview]\n"
           "  configure set storage=<n> enabled=<yes|no> [preview]\n"
           ));
    return false;
  }

  if (Bstrcasecmp(ua->argk[1], NT_("add"))) {
    ConfigChangeGuard guard;
    if (!guard.held()) {
      ua->ErrorMsg(T_("Director busy, try again.\n"));
      return false;
    }
    result = ConfigureAdd(ua, 2);
  } else if (Bstrcasecmp(ua->argk[1], NT_("export"))) {
    result = ConfigureExport(ua);
  } else if (Bstrcasecmp(ua->argk[1], NT_("set"))) {
    result = ConfigureSet(ua);
  } else {
    ua->ErrorMsg(T_("invalid subcommand %s.\n"), ua->argk[1]);
    return false;
  }

  return result;
}
} /* namespace directordaemon */
