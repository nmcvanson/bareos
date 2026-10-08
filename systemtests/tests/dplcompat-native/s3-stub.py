#!/usr/bin/env python3
#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2026-2026 Bareos GmbH & Co. KG
#
#   This program is Free Software; you can redistribute it and/or
#   modify it under the terms of version three of the GNU Affero General Public
#   License as published by the Free Software Foundation and included
#   in the file LICENSE.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#   Affero General Public License for more details.
#
#   You should have received a copy of the GNU Affero General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#   02110-1301, USA.

"""Test-only stub of the S3 calls dplcompat's native transport makes.

It keeps the objects of one bucket as files "<data_dir>/<volume>.<chunk>" (the
layout of the localfile wrapper, so the program transport can read them), checks
the AWS Signature Version 4 of every request independently of the C++ signer
and logs every request. It is a test double for build-tree tests only.

Usage: s3-stub.py <data_dir> <control_dir> <bucket> <access_key> <secret_key>
                  <region>
The port is chosen by the system and written to <control_dir>/port.

Switches (files in <control_dir>, consumed as noted):
  put500        number of PUT requests still answered with 500
  put-badmd5    number of PUT requests still answered with 400 BadDigest
  put-stall     seconds every PUT waits before it is stored and answered
                (writes the key into the file put-stalled when a PUT starts to wait)
  get-stall     seconds every GET waits before it answers
  deny          while the file exists every request gets 403 AccessDenied
Log: <control_dir>/requests.log, one line per request:
  "<method> <path> <status> sig=<ok|bad|none> [range=<value>]"
"""

import base64
import datetime
import hashlib
import hmac
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qsl, quote, unquote, urlsplit

DATA_DIR, CONTROL_DIR, BUCKET, ACCESS_KEY, SECRET_KEY, REGION = sys.argv[1:7]
PAGE_SIZE = 2
LOG_LOCK = threading.Lock()
SWITCH_LOCK = threading.Lock()


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def hmac_sha256(key, msg):
    return hmac.new(key, msg.encode(), hashlib.sha256).digest()


def take_switch(name):
    """True when the counter file has a positive count; counts it down."""
    path = os.path.join(CONTROL_DIR, name)
    with SWITCH_LOCK:
        try:
            with open(path) as f:
                count = int(f.read().strip() or "0")
        except (OSError, ValueError):
            return False
        if count <= 0:
            return False
        with open(path, "w") as f:
            f.write(str(count - 1))
        return True


def switch_value(name):
    try:
        with open(os.path.join(CONTROL_DIR, name)) as f:
            return f.read().strip()
    except OSError:
        return None


def canonical_query(raw_query):
    pairs = [
        (quote(unquote(k), safe="-_.~"), quote(unquote(v), safe="-_.~"))
        for k, v in parse_qsl(raw_query, keep_blank_values=True)
    ]
    return "&".join(f"{k}={v}" for k, v in sorted(pairs))


def expected_signature(handler, body_hash, raw_path, raw_query, fields):
    names = fields["SignedHeaders"].split(";")
    canonical_headers = ""
    for name in names:
        value = " ".join(handler.headers.get(name, "").split())
        canonical_headers += f"{name}:{value}\n"
    request = "\n".join(
        [
            handler.command,
            raw_path,
            canonical_query(raw_query),
            canonical_headers,
            fields["SignedHeaders"],
            body_hash,
        ]
    )
    amz_date = handler.headers.get("x-amz-date", "")
    scope = fields["Credential"].split("/", 1)[1]
    to_sign = "\n".join(
        ["AWS4-HMAC-SHA256", amz_date, scope, sha256_hex(request.encode())]
    )
    date, region, service, terminator = scope.split("/")
    key = hmac_sha256(("AWS4" + SECRET_KEY).encode(), date)
    key = hmac_sha256(key, region)
    key = hmac_sha256(key, service)
    key = hmac_sha256(key, terminator)
    return hmac.new(key, to_sign.encode(), hashlib.sha256).hexdigest()


def xml_error(code, message):
    return (
        f'<?xml version="1.0" encoding="UTF-8"?>\n<Error><Code>{code}'
        f"</Code><Message>{message}</Message></Error>"
    ).encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def log_request_line(self, status, sig, extra=""):
        with LOG_LOCK:
            with open(os.path.join(CONTROL_DIR, "requests.log"), "a") as f:
                f.write(f"{self.command} {self.path} {status} sig={sig}" f"{extra}\n")

    def reply(self, status, body=b"", headers=None, sig="none", extra=""):
        self.send_response(status)
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        self.log_request_line(status, sig, extra)

    def check_signature(self, body):
        """Returns "ok", "bad" or "none"."""
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("AWS4-HMAC-SHA256 "):
            return "none"
        fields = dict(
            part.split("=", 1)
            for part in auth[len("AWS4-HMAC-SHA256 ") :].replace(" ", "").split(",")
        )
        try:
            access_key, scope = fields["Credential"].split("/", 1)
            if access_key != ACCESS_KEY or not scope.endswith("/s3/aws4_request"):
                return "bad"
            if scope.split("/")[1] != REGION:
                return "bad"
            amz_date = self.headers.get("x-amz-date", "")
            sent = datetime.datetime.strptime(amz_date, "%Y%m%dT%H%M%SZ")
            sent = sent.replace(tzinfo=datetime.timezone.utc)
            skew = abs(
                (datetime.datetime.now(datetime.timezone.utc) - sent).total_seconds()
            )
            if skew > 900:
                return "bad"
            body_hash = self.headers.get("x-amz-content-sha256", "")
            if body_hash not in ("UNSIGNED-PAYLOAD", "") and body_hash != sha256_hex(
                body
            ):
                return "bad"
            split = urlsplit(self.path)
            wanted = expected_signature(
                self, body_hash, split.path, split.query, fields
            )
            return "ok" if hmac.compare_digest(wanted, fields["Signature"]) else "bad"
        except (KeyError, ValueError):
            return "bad"

    def object_file(self, key):
        """File of "<bucket>/<volume>/<chunk>", None for any other path."""
        parts = key.strip("/").split("/")
        if len(parts) != 3 or parts[0] != BUCKET:
            return None
        return os.path.join(DATA_DIR, f"{parts[1]}.{parts[2]}")

    def handle_any(self):
        length = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(length) if length else b""
        sig = self.check_signature(body)
        if sig != "ok":
            self.reply(
                403, xml_error("SignatureDoesNotMatch", "bad signature"), sig=sig
            )
            return
        if os.path.exists(os.path.join(CONTROL_DIR, "deny")):
            self.reply(403, xml_error("AccessDenied", "denied"), sig=sig)
            return
        split = urlsplit(self.path)
        path = unquote(split.path)
        query = dict(parse_qsl(split.query, keep_blank_values=True))
        if self.command == "HEAD" and path.strip("/") == BUCKET:
            self.reply(200, sig=sig)
        elif self.command == "GET" and path.strip("/") == BUCKET:
            self.list_objects(query, sig)
        elif self.command == "PUT":
            self.put_object(path, body, sig)
        elif self.command in ("GET", "HEAD"):
            self.get_object(path, sig)
        elif self.command == "DELETE":
            file = self.object_file(path)
            if file and os.path.exists(file):
                os.remove(file)
            self.reply(204, sig=sig)
        else:
            self.reply(405, xml_error("MethodNotAllowed", "no"), sig=sig)

    do_GET = do_PUT = do_HEAD = do_DELETE = handle_any

    def put_object(self, path, body, sig):
        file = self.object_file(path)
        if file is None:
            self.reply(400, xml_error("InvalidRequest", "bad key"), sig=sig)
            return
        stall = switch_value("put-stall")
        if stall:
            with open(os.path.join(CONTROL_DIR, "put-stalled"), "w") as f:
                f.write(path)
            time.sleep(float(stall))
        if take_switch("put500"):
            self.reply(500, xml_error("InternalError", "injected"), sig=sig)
            return
        wanted = self.headers.get("Content-MD5")
        digest = base64.b64encode(hashlib.md5(body).digest()).decode()
        if take_switch("put-badmd5") or (wanted and wanted != digest):
            self.reply(400, xml_error("BadDigest", "md5 mismatch"), sig=sig)
            return
        with open(file + ".part", "wb") as f:
            f.write(body)
        os.replace(file + ".part", file)
        self.reply(200, headers={"ETag": f'"{hashlib.md5(body).hexdigest()}"'}, sig=sig)

    def get_object(self, path, sig):
        file = self.object_file(path)
        if file is None or not os.path.isfile(file):
            self.reply(404, xml_error("NoSuchKey", "no such key"), sig=sig)
            return
        stall = switch_value("get-stall")
        if stall and self.command == "GET":
            time.sleep(float(stall))
        with open(file, "rb") as f:
            data = f.read()
        etag = {"ETag": f'"{hashlib.md5(data).hexdigest()}"'}
        wanted = self.headers.get("Range")
        if wanted and self.command == "GET":
            first, last = wanted.replace("bytes=", "").split("-")
            first, last = int(first), min(int(last), len(data) - 1)
            if first >= len(data):
                self.reply(
                    416,
                    xml_error("InvalidRange", "range"),
                    sig=sig,
                    extra=f" range={wanted}",
                )
                return
            part = data[first : last + 1]
            etag["Content-Range"] = f"bytes {first}-{last}/{len(data)}"
            self.reply(206, part, etag, sig=sig, extra=f" range={wanted}")
            return
        self.send_response(200)
        self.send_header("ETag", etag["ETag"])
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if self.command == "GET":
            self.wfile.write(data)
        self.log_request_line(200, sig)

    def list_objects(self, query, sig):
        prefix = query.get("prefix", "")
        token = query.get("continuation-token", "")
        keys = []
        for name in sorted(os.listdir(DATA_DIR)):
            if name.endswith(".part") or "." not in name:
                continue
            volume, chunk = name.rsplit(".", 1)
            key = f"{volume}/{chunk}"
            if key.startswith(prefix) and key > token:
                keys.append((key, os.path.getsize(os.path.join(DATA_DIR, name))))
        page, rest = keys[:PAGE_SIZE], keys[PAGE_SIZE:]
        out = [
            '<?xml version="1.0" encoding="UTF-8"?>',
            '<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">',
            f"<Name>{BUCKET}</Name><Prefix>{prefix}</Prefix>",
            f"<KeyCount>{len(page)}</KeyCount><MaxKeys>{PAGE_SIZE}</MaxKeys>",
            f"<IsTruncated>{'true' if rest else 'false'}</IsTruncated>",
        ]
        for key, size in page:
            out.append(f"<Contents><Key>{key}</Key><Size>{size}</Size></Contents>")
        if rest:
            out.append(
                f"<NextContinuationToken>{page[-1][0]}" "</NextContinuationToken>"
            )
        out.append("</ListBucketResult>")
        self.reply(
            200, "".join(out).encode(), {"Content-Type": "application/xml"}, sig=sig
        )


def main():
    os.makedirs(DATA_DIR, exist_ok=True)
    os.makedirs(CONTROL_DIR, exist_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    with open(os.path.join(CONTROL_DIR, "port.tmp"), "w") as f:
        f.write(str(server.server_address[1]))
    os.replace(os.path.join(CONTROL_DIR, "port.tmp"), os.path.join(CONTROL_DIR, "port"))
    server.serve_forever()


if __name__ == "__main__":
    main()
