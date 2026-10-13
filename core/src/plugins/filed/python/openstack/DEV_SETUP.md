# Dev/Test Environment

Companion to [`ROADMAP.md`](ROADMAP.md) — how to get a working local setup
for coding and manually validating `bareos-fd-openstack` against a real
cloud (DESIGN.md §12.2's "manual/staging validation" step, and the M1
throwaway PoC script in §14). Nothing here ships in the plugin package;
it's all gitignored dev tooling (`.gitignore` in this directory).

## 1. Test cloud

Development and manual validation use a private OpenStack all-in-one lab
cluster (not part of this repo, not publicly reachable info) — its
endpoint, credentials, and current instance/volume inventory are kept in
each developer's own local notes / password manager, not in this file.

| | |
|---|---|
| Keystone endpoint | `<your-openstack-endpoint>/v3` (needs Identity v3) |
| Credentials | in local `clouds.yaml` (gitignored — **not** committed), cloud name `bareos-lab` |
| Auth check | `curl -X POST <endpoint>/v3/auth/tokens -H "Content-Type: application/json" -d '{"auth":{"identity":{"methods":["password"],"password":{"user":{"name":"<user>","domain":{"id":"default"},"password":"<pwd>"}}}}}'` → `201` with an `X-Subject-Token` header confirms it's up |

Minimum service catalog needed: `identity`, `compute` (Nova), `volumev3`
(Cinder), `image` (Glance) — everything §5/§6 of `DESIGN.md` needs.

### 1.1 Inventory you'll need

For manual testing you need, at minimum:
- One **target instance** to back up — ideally boot-from-volume, since
  that's the primary path DESIGN.md §5 covers, with at least one extra
  data volume attached.
- One **proxy instance** — a separate Nova instance (not the target) that
  the plugin's FD would run on, since DESIGN.md §4/§5 requires the proxy
  to itself be attachable-to as a Cinder volume target. Its instance UUID
  is the value for `proxy_instance_id=`.

A single-project, single-AZ all-in-one cloud is enough for M1/M2
development, but not for the multi-project/cross-project restore cases in
§6/§12.1 — those need a second project or a second lab cloud.

## 2. Local Python environment

A per-plugin venv, not the system Python, since `openstacksdk` isn't
something to install globally:

```
cd core/src/plugins/filed/python/openstack
python -m venv .venv
.venv/Scripts/python.exe -m pip install -r requirements-dev.txt   # Windows
# .venv/bin/python -m pip install -r requirements-dev.txt         # Linux/macOS
```

Installed and verified working here: `openstacksdk` 4.20.0, `keystoneauth1`
5.17.0, `requests_mock` 1.12.1 (for §12.2's fake-backed unit tests).

## 3. Connecting

Two ways, both verified against the lab cloud above:

**a) Explicit kwargs** (what a throwaway M1 script can use directly):

```python
import openstack
conn = openstack.connect(
    auth_url="<your-openstack-endpoint>/v3",
    username="<user>",
    password="<pwd>",  # from local clouds.yaml, don't hardcode in real scripts
    project_name="<project>",
    user_domain_name="Default",
    project_domain_name="Default",
    interface="public",
)
```

**b) `cloud=` name via `clouds.yaml`** (what the plugin's own `cloud=`
option, DESIGN.md §4/§10, resolves at runtime):

```python
import openstack
conn = openstack.connect(cloud="bareos-lab")
```

For (b) to resolve without copying the file around, either run from this
directory (`clouds.yaml` is discovered in the CWD) or point
`OS_CLIENT_CONFIG_FILE` at it, or copy it to
`~/.config/openstack/clouds.yaml` (Linux/macOS) /
`%USERPROFILE%\.config\openstack\clouds.yaml` (Windows) — standard
`openstacksdk` config discovery locations.

## 4. Sanity script

A one-off inventory probe (auth + list servers/flavors/volumes/images/
networks/AZs) was used to produce §1.1's inventory above and confirm SDK
connectivity end-to-end. Not checked in (it's a throwaway, same spirit as
the M1 PoC script in DESIGN.md §14) — recreate it ad hoc with
`openstack.connect(cloud="bareos-lab")` plus the relevant proxy calls
(`conn.compute.servers()`, `conn.block_storage.volumes()`,
`conn.image.images()`, `conn.network.networks()`,
`conn.compute.availability_zones()`) whenever a fresh inventory check is
needed.

## 5. Security notes

- `clouds.yaml` and `.venv/` in this directory are gitignored — never
  remove those `.gitignore` entries or commit real credentials.
- Per DESIGN.md §4: the plugin itself must never pass secrets to
  `bareosfd.JobMessage()`; only `bareosfd.DebugMessage()`. Keep the same
  discipline in any dev/debug scripts against this lab cloud.
