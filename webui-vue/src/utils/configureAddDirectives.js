/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026 Bareos GmbH & Co. KG

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


// Flat directive names and types from dird_conf.cc, tls_resource_items.h and
// messages_resource_items.h; quoting mirrors config_add_directive.

const job = {
  quoted: [
    'Name', 'Description', 'BackupFormat', 'Messages', 'StorageGroupPolicy', 'Pool', 'FullBackupPool',
    'VirtualFullBackupPool', 'IncrementalBackupPool', 'DifferentialBackupPool', 'NextPool', 'Client',
    'FileSet', 'Schedule', 'JobToVerify', 'Catalog', 'JobDefs', 'Where', 'RegexWhere', 'StripPrefix',
    'AddPrefix', 'AddSuffix', 'Bootstrap', 'WriteBootstrap', 'RunBeforeJob', 'RunAfterJob',
    'RunAfterFailedJob', 'ClientRunBeforeJob', 'ClientRunAfterJob', 'SelectionPattern',
  ],
  bare: [
    'Type', 'Protocol', 'Level', 'Storage', 'Run', 'Replace', 'MaximumBandwidth', 'MaxRunSchedTime',
    'MaxRunTime', 'FullMaxRuntime', 'IncrementalMaxRuntime', 'DifferentialMaxRuntime', 'MaxWaitTime',
    'MaxStartDelay', 'MaxFullInterval', 'MaxVirtualFullInterval', 'MaxDiffInterval', 'PrefixLinks',
    'PruneJobs', 'PruneFiles', 'PruneVolumes', 'PurgeMigrationJob', 'Enabled', 'SpoolAttributes',
    'SpoolData', 'SpoolSize', 'RerunFailedLevels', 'PreferMountedVolumes', 'MaximumConcurrentJobs',
    'RescheduleOnError', 'RescheduleInterval', 'RescheduleTimes', 'Priority', 'AllowMixedPriority',
    'SelectionType', 'Accurate', 'AllowDuplicateJobs', 'AllowHigherDuplicates', 'CancelLowerLevelDuplicates',
    'CancelQueuedDuplicates', 'CancelRunningDuplicates', 'SaveFileHistory', 'FileHistorySize',
    'FdPluginOptions', 'SdPluginOptions', 'DirPluginOptions', 'MaxConcurrentCopies', 'AlwaysIncremental',
    'AlwaysIncrementalJobRetention', 'AlwaysIncrementalKeepNumber', 'AlwaysIncrementalMaxFullAge',
    'MaxFullConsolidations', 'RunOnIncomingConnectInterval',
  ],
}

const client = {
  quoted: [
    'Name', 'Description', 'Address', 'LanAddress', 'Username', 'Password', 'Catalog', 'TlsCipherList',
    'TlsCipherSuites', 'TlsDhFile', 'TlsProtocol', 'TlsCaCertificateFile', 'TlsCaCertificateDir',
    'TlsCertificateRevocationList', 'TlsCertificate', 'TlsKey',
  ],
  bare: [
    'Protocol', 'AuthType', 'Port', 'Passive', 'ConnectionFromDirectorToClient',
    'ConnectionFromClientToDirector', 'Enabled', 'HardQuota', 'SoftQuota', 'SoftQuotaGracePeriod',
    'StrictQuotas', 'QuotaIncludeFailedJobs', 'FileRetention', 'JobRetention', 'HeartbeatInterval',
    'AutoPrune', 'MaximumConcurrentJobs', 'MaximumBandwidthPerJob', 'NdmpLogLevel', 'NdmpBlockSize',
    'NdmpUseLmdb', 'TlsAuthenticate', 'TlsEnable', 'TlsRequire', 'TlsVerifyPeer', 'TlsAllowedCn',
  ],
}

const storage = {
  quoted: [
    'Name', 'Description', 'Address', 'LanAddress', 'Username', 'Password', 'MediaType', 'PairedStorage',
    'NdmpChangerDevice', 'TlsCipherList', 'TlsCipherSuites', 'TlsDhFile', 'TlsProtocol',
    'TlsCaCertificateFile', 'TlsCaCertificateDir', 'TlsCertificateRevocationList', 'TlsCertificate',
    'TlsKey',
  ],
  bare: [
    'Protocol', 'AuthType', 'Port', 'Device', 'AutoChanger', 'Enabled', 'AllowCompression',
    'HeartbeatInterval', 'CacheStatusInterval', 'MaximumConcurrentJobs', 'MaximumConcurrentReadJobs',
    'MaximumBandwidthPerJob', 'CollectStatistics', 'TlsAuthenticate', 'TlsEnable', 'TlsRequire',
    'TlsVerifyPeer', 'TlsAllowedCn',
  ],
}

const pool = {
  quoted: [
    'Name', 'Description', 'LabelFormat', 'LabelType', 'CleaningPrefix', 'NextPool', 'StorageGroupPolicy',
    'RecyclePool', 'ScratchPool', 'Catalog',
  ],
  bare: [
    'PoolType', 'UseCatalog', 'PurgeOldestVolume', 'ActionOnPurge', 'RecycleOldestVolume',
    'RecycleCurrentVolume', 'MaximumVolumes', 'MaximumVolumeJobs', 'MaximumVolumeFiles',
    'MaximumVolumeBytes', 'CatalogFiles', 'VolumeRetention', 'VolumeUseDuration', 'MigrationTime',
    'MigrationHighBytes', 'MigrationLowBytes', 'Storage', 'AutoPrune', 'Recycle', 'FileRetention',
    'JobRetention', 'MinimumBlockSize', 'MaximumBlockSize',
  ],
}

const schedule = {
  quoted: [
    'Name', 'Description',
  ],
  bare: [
    'Run', 'Enabled',
  ],
}

const messages = {
  quoted: [
    'Name', 'Description', 'MailCommand', 'OperatorCommand', 'TimestampFormat',
  ],
  bare: [
    'Syslog', 'Mail', 'MailOnError', 'MailOnSuccess', 'File', 'Append', 'Stdout', 'Stderr', 'Director',
    'Console', 'Operator', 'Catalog',
  ],
}

export const NATIVE_DIRECTIVES = Object.freeze({ job, jobdefs: job, client, storage, pool, schedule, messages })
