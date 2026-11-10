# Hosting on Cloudflare (spike S4a)

Result: a Worker, an R2 bucket and an Access application are deployed and work together; a publisher cuts
game files into content-addressed chunks and uploads them. All of `db/` (67 files, 15.97 GiB, 4,128 chunks)
is in the bucket ([numbers](#result-for-all-of-db)), and `verify` finds nothing missing. The unauthenticated
check passes (every path answers with the Access login redirect and no content). The owner's logged-in check
passed except for one expectation, `cf-cache-status`, which is an [open item](#open-item-cf-cache-status-shows-na).

Deployment values (account id, hostname, login email, Access team name) are not in the repository. This
document writes them as `<hostname>`, `<team>` and so on; the real ones live in the untracked
`web/worker/deploy.local.env`.

## Architecture

```
browser ──► Cloudflare Access (allow-list of emails, whole hostname)
               └─► Worker "anomaly-web-worker" on a custom domain
                     ├─ /            static assets  (web/worker/public)
                     ├─ /data/*      R2 bucket "anomaly-web-data", key = path without the leading slash
                     └─ /bin/*       same bucket (wasm binary, shader pack later)
```

- **One origin.** The page, the data and the binary come from the same hostname, so there is no CORS and the
  isolation headers cover everything.
- **Access runs first.** Cloudflare Access sits in front of the whole hostname; the Worker authenticates
  nobody and has no secrets. The R2 bucket has no public access: no `r2.dev` address, no custom domain. The
  Worker's `workers.dev` address and preview URLs are off (`workers_dev` and `preview_urls` are `false` in
  the config), so the only way to the Worker is through the Access-protected hostname.
- **Every response** from the Worker carries `Cross-Origin-Opener-Policy: same-origin`,
  `Cross-Origin-Embedder-Policy: require-corp`, `Cross-Origin-Resource-Policy: same-origin` and
  `X-Content-Type-Options: nosniff`, including 304, 404, 405, 416 and 500. The Worker runs first for static
  assets too (`run_worker_first`), so the page gets them as well. Cloudflare's own `/cdn-cgi/*` paths and the
  Access login pages are not served by the Worker and do not carry them.
- **Types.** `.wasm` is `application/wasm` whatever the object was uploaded with (the extension wins),
  `.json` is `application/json`, chunks are `application/octet-stream`.
- **Caching.** `data/chunks/<sha256>` is `public, max-age=31536000, immutable` and is stored in the edge
  cache (Cache API) on first read; later reads, including `Range` and `If-None-Match`, are answered from it.
  `data/index.json` and unhashed `/bin/*` files are `no-cache`: always revalidated, cheap because of the
  `ETag`. A `/bin/` file whose name holds a content hash (`name.<12 to 64 hex>.ext`) is treated as immutable.
- **Methods.** `GET` and `HEAD` (with `ETag`, `Last-Modified`, `Accept-Ranges`, `Content-Length`);
  `If-None-Match` gives 304 on both; a single `Range` gives 206; a range past the end gives 416; anything
  else on `/data` or `/bin` is 405. Object names are checked against a strict pattern, so `..`, empty
  segments and encoded slashes are 404.

### R2 layout and index

```
data/chunks/<sha256 hex>     one object per distinct chunk, immutable
data/index.json              written last, after every chunk it lists is stored
bin/...                      not used by this spike
```

Files are cut into fixed-size chunks (default 4 MiB, `--chunk-size`; the last chunk of a file is shorter).
Identical chunks, within a file or across files, are stored once. `index.json` (compact JSON):

| Field | Meaning |
|---|---|
| `format` | `anomaly-web-index/1` |
| `chunkSize` | bytes per chunk |
| `chunkPath` | `chunks/`, relative to the index URL, so a chunk is at `/data/chunks/<hash>` |
| `totalSize` | sum of file sizes |
| `files[].path` | path below the game directory, forward slashes (`db/configs/configs.db0`) |
| `files[].size`, `.sha256` | size and SHA-256 of the whole file |
| `files[].chunks` | SHA-256 of each chunk in order; byte `n` is in chunk `n / chunkSize` |
| `files[].archive` | present for `.db*` / `.xdb*` files, see below |

**Archive table of contents.** For each X-Ray archive the publisher reads the chunk directory and decodes the
table of contents, so the engine can register every file in the archive from the index alone, without opening
the archive. The format is the one `CLocatorAPI::LoadArchive` and `open_chunk_fd` read
(`src/xrCore/LocatorAPI.cpp:321-357`, `:455-510`; LZHUF as in `src/xrCore/lzhuf.cpp`):

```json
"archive": {
  "nameEncoding": "latin-1",
  "entryPoint": "$fs_root$\\gamedata\\",
  "autoLoad": true,
  "header": "[header]\r\nauto_load = true\r\n...",
  "chunks": [[666,0,8,155], [0,0,171,1654963], [1,1,1655142,9769]],
  "entries": [["scripts\\_g.script", 171, 98700, 29156, 2119669721], ...]
}
```

- `header`: the text of chunk 666, which `ProcessArchive` feeds to `CInifile`; `entryPoint` and `autoLoad`
  are the two keys the engine reads from it.
- `entries`: one row per record of chunk 1, in the order `LoadArchive` passes them to `Register`:
  name, absolute byte offset in the archive (`ptr`), real size, stored size, CRC. Directory records have
  offset 0 and size 0. A file's bytes are `[offset, offset + stored size)` of the archive, so the web file
  backend maps that range onto chunks.
- `chunks`: the archive's own chunk directory as `[id, LZ-compressed, payload offset, payload size]`.
- `nameEncoding`: names are the raw bytes of the Windows code page. Two stock archives
  (`textures.db2`, `textures_weapons.db0`) contain Cyrillic names, so each name character stands for one
  byte (`ord(c)`), not for a Unicode letter. An earlier version of the publisher assumed ASCII and failed on
  those two files; the test is in `test_publish.py`.

Size of the index for the sample: 337 KiB. For all of `db/` (67 files, 15.97 GiB, 67 archives, 50,781 table
entries, 4,128 chunks) the stored `index.json` is 4,686,956 bytes (4.47 MiB); gzip -6 gives 1,121,696 bytes
(1.07 MiB), gzip -9 1,091,733, brotli quality 5 1,008,383 (0.96 MiB). That is one request, but a start-up
cost worth a look at the W1 milestone; the fallback is one small index per archive, loaded when that archive
is first needed. Whether Cloudflare compresses the response at the edge is not verified.

## Tooling

| Tool | Version | Used for |
|---|---|---|
| `cf` | **1.0.0-beta.28** | bucket, Access, reading account state, every R2 object operation (list, put, get), local simulation of R2 |
| `wrangler` | 4.159.0 | `wrangler deploy` and `wrangler dev` of the Worker |
| Python | 3.9 or newer, standard library only | publisher and its tests |
| Node | 22 or newer | Worker unit tests, `smoke.mjs` |

Why beta.28 and not beta.20: beta.20 was the zsh default, beta.28 is the newer one, and beta.20 is queued
for removal by the version manager. beta.28 ran every `cf` command below. Point the scripts at it with
`CF_BIN` in `deploy.local.env`; `publish.py` also reads `$CF_BIN`, or take it from `--cf`. The CLI is a beta
and its output format is not a contract (`r2 objects list` prints the page as JSON on stdout and the cursor
on stderr; `objects get` writes the raw body to stdout): pin the version and rerun
`python3 -I -m unittest discover -s tools/web/publish -p 'test_*.py'` after changing it.

**Where `wrangler` was needed.** `cf deploy` in beta.28 does not read `wrangler.jsonc`; given this folder it
treated it as a static site, ignoring `src/worker.js`, the R2 binding and the custom domain. Its own format,
`cloudflare.config.ts`, is in open beta and has no field for the assets directory or `run_worker_first`. So
the Worker is deployed with `wrangler deploy`. Everything else, including bucket creation and the Access
application, goes through `cf`. `cf dev` was not tried; local development uses `wrangler dev`.

No API tokens were created. Both CLIs use the OAuth logins already on the machine.

## Files

| Path | What |
|---|---|
| `web/worker/src/worker.js`, `policy.js` | the Worker; `policy.js` holds the pure rules (paths, cache lifetimes, types) |
| `web/worker/public/` | placeholder page: isolation flags, index, 4 chunks hash-verified, HEAD, 304, Range, 404 |
| `web/worker/wrangler.example.jsonc` | deployment template; `scripts/deploy.sh` renders it to `wrangler.jsonc` (untracked) |
| `web/worker/wrangler.dev.jsonc` | local development, no routes |
| `web/worker/deploy.local.env.example` | settings template; copy to `deploy.local.env` (untracked) |
| `web/worker/scripts/` | `setup-access.sh`, `deploy.sh`, `common.sh` |
| `web/worker/test/` | `policy.test.js` (unit), `smoke.mjs` (black-box against a running Worker) |
| `tools/web/publish/` | `publish.py`, `xray_archive.py`, `test_publish.py` |

## Local development

`cf --local` and `wrangler dev` share one simulated bucket when given the same `--persist-to` directory, so
the local loop needs no account and no network:

```bash
ROOT=$(git rev-parse --show-toplevel)
STATE=$ROOT/build/web-local-state            # build/ is ignored by git
GAME=/path/to/Anomaly                        # the directory with db/, gamedata/, ...

# 1. fill the simulated bucket from game files (the sample; use any --path)
python3 -I $ROOT/tools/web/publish/publish.py publish --source "$GAME" --path db/configs \
  --bucket anomaly-web-local --local --persist-to "$STATE"

# 2. serve the Worker on http://127.0.0.1:8787
cd $ROOT/web/worker && wrangler dev -c wrangler.dev.jsonc --persist-to "$STATE"

# 3. in another terminal: black-box checks, then open the URL in Chrome
node $ROOT/web/worker/test/smoke.mjs http://127.0.0.1:8787
```

Unit tests: `cd web/worker && npm test` (or `node --test test/policy.test.js`) and
`python3 -I -m unittest discover -s tools/web/publish -p 'test_*.py'` (about 20 s; the LZ round-trip test
uses a slow reference encoder).

To run the same Worker code against the **real bucket** without deploying anything, give the R2 binding
`"remote": true` in a copy of `wrangler.dev.jsonc` (with `bucket_name` set to the real bucket) and run
`wrangler dev` on that copy. Reads then go to R2 over the API; use it read-only. This is how the Worker was
checked against production R2 (see "Limits and surprises").

## Deploy

One-time setup. The order matters: the bucket and the Access application exist before the Worker, and the
Worker is deployed before any data is uploaded.

```bash
cd web/worker
cp deploy.local.env.example deploy.local.env     # set ANOMALY_WEB_HOST, ANOMALY_WEB_BUCKET (anomaly-web-*), CF_BIN
. ./deploy.local.env

# 1. private bucket: no public access, no r2.dev address (both are the default)
$CF_BIN r2 buckets create-by-name "$ANOMALY_WEB_BUCKET"
$CF_BIN r2 buckets domains managed get "$ANOMALY_WEB_BUCKET"      # expect "enabled": false

# 2. Access application for the whole hostname, one allow policy with only your login email
scripts/setup-access.sh --dry-run                                  # prints the request, email masked
scripts/setup-access.sh

# 3. Worker on the custom domain (refuses to run unless step 2 exists)
scripts/deploy.sh --dry-run
scripts/deploy.sh
```

What each script does:

- `setup-access.sh` creates `anomaly-web-access` (self-hosted, destination `<hostname>` with no path, 24 h
  session, HTTP-only cookie, no preflight bypass) with one inline policy `anomaly-web-allow`
  (decision allow, include one `email` rule). The email is read from `cf auth whoami` at run time and goes
  straight into the request; it is not written to a file. If the account has a Cloudflare-login identity
  provider, only that one is allowed, with auto-redirect. It does nothing when an
  application with that name exists.
- `deploy.sh` renders `wrangler.jsonc` from the template (hostname and bucket from the env file), checks that
  the Access application exists, and runs `wrangler deploy -c wrangler.jsonc`. The custom domain makes
  Cloudflare create the DNS record for the hostname (an `AAAA 100::` record, proxied, read-only, owned by the
  Worker) and a certificate; no other DNS change is made. After a deploy, confirm both off-switches:
  `cf workers get anomaly-web-worker` shows `"subdomain": {"enabled": false, "previews_enabled": false, ...}`.

Redeploying after a code change is `scripts/deploy.sh` again (about 12 s).

### Unauthenticated check (step 4), recorded

Run from a machine with no Access session. Every path, including `/data/` and `/bin/`, answers `302` to the
Access login page and no content; the body is the generic 143-byte redirect page.

```console
$ curl -sI https://<hostname>/data/index.json
HTTP/2 302
content-type: text/html; charset=UTF-8
location: https://<team>.cloudflareaccess.com/cdn-cgi/access/login/<hostname>?kid=...&redirect_url=%2Fdata%2Findex.json
www-authenticate: Cloudflare-Access resource_metadata="https://<hostname>/.well-known/cloudflare-access-protected-resource/data/index.json"
cache-control: private, max-age=0, no-store, no-cache, must-revalidate, post-check=0, pre-check=0
server: cloudflare

$ for p in / /index.html /app.js /style.css /data/ /data/index.json /data/chunks/<64 zeros> /bin/ /bin/anomaly.wasm /nope; do curl -s -o /dev/null -w "%{http_code} $p\n" https://<hostname>$p; done
302 /          302 /index.html   302 /app.js       302 /style.css    302 /data/
302 /data/index.json   302 /data/chunks/<64 zeros>   302 /bin/   302 /bin/anomaly.wasm   302 /nope
$ curl -s -X POST -o /dev/null -w "%{http_code}\n" https://<hostname>/data/index.json
302
$ curl -sI http://<hostname>/data/index.json | head -1
HTTP/1.1 301 Moved Permanently          # plain HTTP is redirected to https, then to the login
$ curl -sI https://anomaly-web-worker.<account>.workers.dev/ | head -1
HTTP/2 404                              # workers.dev address is off
```

The check was repeated after the second deploy (`/`, `/data/index.json`, `/bin/` all 302). One path is
served without Access: `/cdn-cgi/trace` answers 200, because it is Cloudflare's own edge diagnostic
endpoint (it shows the caller's own connection details, nothing of ours).

### Owner check, after logging in

**Passed, with one open item.** The owner signed in at the deployed origin on 2026-11-10 and reported (to the
root session, which relayed it; I did not see the page) steps 1 to 3 as expected: `crossOriginIsolated`
true, `SharedArrayBuffer`, WebGPU, WebHID and OPFS present, "All checks passed" (`index.json` 200, four
chunks hash-verified, HEAD 200, If-None-Match 304, Range 206, missing chunk 404). Step 4 did not give what it
expected: every chunk row showed `cf-cache-status n/a`, see the
[open item](#open-item-cf-cache-status-shows-na). Steps 5 and 6 were not reported.

I could not do this part myself: it needs a browser session for the allow-listed email. The steps:

1. Open `https://<hostname>/` in desktop Chrome or Edge. Expect the Cloudflare Access login for your account,
   then the page "Hosting check".
2. In the "Browser" box `crossOriginIsolated` must be `true` and `SharedArrayBuffer` `available`. If not,
   open DevTools, Network, click the document request and check the three isolation response headers.
3. "Data" must end with "All checks passed.": `index.json` 200, 4 chunks "hash verified", HEAD 200,
   If-None-Match 304, Range 206, missing chunk 404.
4. Reload. I expected the chunk rows to show `cf-cache-status: HIT` once the edge cache is warm. That
   expectation was wrong for this Worker: it never sets that header (see the open item), so the row says `n/a`.
5. In a private window, or after clearing the `CF_Authorization` cookie, `https://<hostname>/data/index.json`
   must go back to the login page, not return JSON.
6. Optional: from a second account whose email is not on the list, the login must be refused.

## Publish

```bash
cd web/worker && . ./deploy.local.env && cd ../..
python3 -I tools/web/publish/publish.py publish --source "$GAME" --path db/configs \
  --bucket "$ANOMALY_WEB_BUCKET" --cf "$CF_BIN" --dry-run        # plan and compare with the bucket
python3 -I tools/web/publish/publish.py publish --source "$GAME" --path db/configs \
  --bucket "$ANOMALY_WEB_BUCKET" --cf "$CF_BIN"                  # upload
python3 -I tools/web/publish/publish.py verify --bucket "$ANOMALY_WEB_BUCKET" --cf "$CF_BIN" --deep
```

`--path` may repeat and names a file or a directory below `--source`; leave it out for everything. Options:
`--chunk-size` (default `4MiB`, at least 64 KiB), `--jobs` (parallel `cf` processes, at most 8), `--retries`
(default 3 extra attempts per chunk), `--prefix` (default `data`), `--work-dir` (default `build/web-publish`),
`--local --persist-to DIR` (the local simulation), `--no-cache`, `--dry-run`.

How it behaves:

1. Walks the selected paths (regular files only: dot files and symlinks are skipped, `--path` may not leave
   `--source`), hashes every file in 4 MiB chunks and decodes archive tables. Results are cached in
   `<work-dir>/plan-cache.json` by path, size, mtime and chunk size, so a rerun does not read 16 GB again.
2. Lists `data/chunks/` once (paged with `--start-after`) and uploads only chunks that are absent or have the
   wrong size. A chunk is re-hashed from the source file right before it is uploaded, so a file that changed
   in between aborts that chunk instead of storing wrong bytes under a hash.
3. Uploads with up to 8 `cf r2 objects put` processes, retrying with backoff. If any chunk fails it reports
   them, **does not upload the index**, and exits 1; running the same command again continues where it
   stopped. An object only appears once its upload is complete, so an interrupted upload leaves no partial
   chunks.
4. Uploads `index.json` last, and only if its MD5 differs from the stored object's ETag.

Temporary files are 4 MiB chunk copies in a `publish-*` directory under the work directory, each removed
after its upload; the directory is removed with a marker file check and a non-recursive `rmdir`.
`verify` compares the stored index with the stored chunks (existence and size) and, with `--deep`, downloads
every chunk and compares its SHA-256.

Not implemented: pruning chunks no index references any more. Old versions' chunks stay until removed by
hand; at about $0.015 per GB-month that is cheap, and a `prune` command can follow when the data changes.

### Result for the sample

| Run (real bucket) | Chunks uploaded | Index | Wall time |
|---|---|---|---|
| 1: first publish of `db/configs` (4 files, 24.4 MiB, 9 chunks) | 9 (24.4 MiB) | uploaded | 11.3 s (3.4 MiB/s) |
| 2: same command again | 0, 9 skipped | unchanged, skipped | 2.5 s |
| 3: after a change to the index format (names as latin-1) | 0, 9 skipped | uploaded | n/a |
| 4: same command again | 0, 9 skipped | unchanged, skipped | n/a |
| `verify --deep` | 9 downloaded, hashed, 0 problems | | 7.5 s |

Runs 3 and 4 exist because the index format changed after run 2; run 4 is the "second run uploads nothing"
for the final format. The full output is in the session notes folder, not in the repository.

### Result for all of `db/`

Run on 2026-11-10 with the owner's go-ahead, after re-checking that the bucket was still private (`r2.dev`
off, no custom domains) and that an unauthenticated `curl -sI` on `/data/index.json`, a chunk path, `/`
and `/bin/` still gave the Access redirect. Source: the game directory, read only. Command:
`publish --path db --jobs 8` (4 MiB chunks), one background job, started after a `--dry-run` whose plan it
then matched exactly.

| | |
|---|---|
| Files, size | 67 (`db/` only), 15.97 GiB (17,154,171,163 bytes in the bucket including the index) |
| Chunks | 4,128 referenced, 4,128 distinct (no chunk is shared between files), 4,119 uploaded, 9 skipped (already stored by the sample run) |
| Wall time | 1,894 s (31 min 34 s), 02:23:33Z to 02:55:07Z, one run, no resume needed |
| Average rate | 8.6 MiB/s (15.9 GiB uploaded in 1,894 s); the running average in the progress lines stayed between 6.9 and 8.7 MiB/s |
| Failures | 0 chunks failed after retries; the index was uploaded last |
| Retries | not counted: the publisher logs a chunk only when all attempts failed, so a retry that succeeded leaves no trace. A counter would be a small addition |
| `index.json` | 4,686,956 bytes (4.47 MiB); gzip -6 1,121,696; brotli q5 1,008,383 |
| Bucket afterwards | 4,129 objects (4,128 chunks + `data/index.json`), 17,154,171,163 bytes (15.976 GiB) |

Checks afterwards:

- `verify` (existence and size of every chunk the index lists): 4,128 chunks, 0 missing or wrong size, 7 s.
- Deep sample (an ad hoc script, not part of the repository, using the publisher's `cf r2 objects get`
  wrapper; the publisher's own `verify --deep` has no sample option and would download all 16 GB):
  two whole files, `db/levels/level_pripyat.db0` (485.4 MiB, 122 chunks) and `db/textures/textures_ui.db0`
  (63.6 MiB, 16 chunks), each downloaded, every chunk hashed against its key, reassembled, and the file's
  SHA-256 compared with the index and with the local source (equal); plus 150 random chunks from 48 other
  files. 288 chunks (about 1.1 GiB), 0 problems, 115 s. `level_pripyat.db0` is larger than 300 MB, the most
  `cf r2 objects put` takes in one object, so it exists in the bucket only as chunks.
- Unauthenticated `curl -sI` after the upload: `/data/index.json`, a chunk path, `/`, `/bin/` and `/anything`
  all give 302 to the Access login, and `/data/index.json` returns the generic 143-byte body. `r2.dev`
  still `enabled: false`, custom domains empty.

The 8-way upload is bounded by the `cf` start-up per object, as expected: 4,119 objects in 1,894 s is 2.2
objects per second. That is acceptable for a first publish and for changed chunks later (only chunks whose
bytes differ are uploaded; a change that shifts the following bytes of a file changes the rest of its
chunks). The S3 API would be faster but needs an R2 API token, which this spike was not allowed to create.

## R2 operations and cost observed

Counted from the commands run against the real bucket (a hand count of the publisher's `cf` calls, not
billing data). Sample phase: bucket creation, one dry run, four publish runs, two `verify --deep`, plus a
read-only development session against it. Full upload: one dry-run list, the publish (one list, 4,119 chunk
puts, one index put), `verify` (five list pages), a bucket-wide list for the object count (five pages), the
deep sample (one index get, 288 chunk gets) and the shallow verify's index get.

| | Sample phase | Full upload | Free tier per month |
|---|---|---|---|
| Class A (list, put, create bucket) | about 25 | about 4,130 | 1,000,000 |
| Class B (get, head) | under 100 | about 290 | 10,000,000 |
| Storage | 24.4 MiB of chunks + 0.3 MiB index | 15.976 GiB (17.15 GB) | 10 GB-month |
| Egress | 0 | 0 (the deep sample's downloads included; free for R2 in any case) | |

So the operations cost nothing: about 0.4 % of the Class A allowance. Prices from the R2 pricing page, read
on 2026-11-10: storage $0.015 per GB-month, Class A $4.50 per million, Class B $0.36 per million, egress
free. Storage above the free 10 GB is $0.09 a month counting GiB and $0.11 counting 10^9 bytes; I did not
check which unit the bill uses. A publish costs one list per 1,000 stored chunks plus one put per new
chunk and one for the index, regardless of how many files they belong to, so a republish of unchanged data
is about 6 Class A operations.

Request counts matter more than R2 operations at play time. With `run_worker_first`, every request, static
asset or chunk, runs the Worker. The Workers Free plan allows 100,000 requests a day (Error 1027 beyond that);
a full download of `db/` is about 4,100 requests, so about 24 complete downloads a day. I did not check which
Workers plan this account is on. If the free limit is a problem, the Workers Paid plan has no daily limit, and
chunks could be larger (the chunk size is a publisher parameter).

## Invite a person

Access decides, so no Worker change is needed. In the Cloudflare dashboard: Zero Trust, Access, Applications,
`anomaly-web-access`, Policies, `anomaly-web-allow`, add an include rule of type "Emails" with the person's
address, save. The next visit offers them the login (the identity provider on the application is the
Cloudflare account login unless you changed it; for people without a Cloudflare account, add another identity
provider such as one-time PIN to the application first). Remove the rule to revoke; existing sessions last up
to 24 hours. A `cf`-only route (`cf zero-trust access applications update`) exists but replaces the whole
application, so I did not script it; I also did not exercise the dashboard steps, which is a UI I cannot see.

## Tear down

Not run; the `--dry-run` output of the delete commands was read and the requests are the expected ones. In this order:

```bash
. web/worker/deploy.local.env
# 1. Worker and its custom domain (the AAAA record is owned by the Worker and goes with it; check afterwards)
$CF_BIN workers delete anomaly-web-worker --dry-run && $CF_BIN workers delete anomaly-web-worker
$CF_BIN dns records list --zone <zone>            # no record for <hostname> should remain; else delete it by id
# 2. Access application (id from the list)
$CF_BIN zero-trust access applications list       # find anomaly-web-access
$CF_BIN zero-trust access applications delete <app-id>
# 3. empty the bucket (a background job; poll it with `cf r2 buckets jobs get`), then delete it
$CF_BIN r2 buckets jobs create "$ANOMALY_WEB_BUCKET" --job-type prefixDelete --prefix "" --dry-run
$CF_BIN r2 buckets delete "$ANOMALY_WEB_BUCKET"
```

Worker first: deleting the Access application while the Worker still serves would open the data to
everyone. `r2 buckets delete` needs an empty bucket, which the prefix-delete job with an empty prefix
provides. Nothing else was created: no tokens, no extra DNS records, no other
Access applications, policies, buckets or Workers. Locally, remove `web/worker/deploy.local.env`,
`web/worker/wrangler.jsonc` and `build/web-publish`.

## Limits and surprises

- **`cf deploy` ignores `wrangler.jsonc`** in beta.28 (see Tooling). `wrangler deploy` was used.
- **R2 throws on an unsatisfiable `Range`** (`10039`) instead of returning a result, but only for objects
  that are not in the edge cache; for cached ones the Cache API answers 416 itself. The first version of the
  Worker returned 500 for a cold chunk. It now answers 416 with `Content-Range: bytes */size`. Found by
  running the black-box test against real R2; the local simulation does not reproduce it, so keep running
  `smoke.mjs` against a Worker with a remote R2 binding after Worker changes. R2 ignores malformed or
  multi-range headers and returns the full object (200).
- **`cf r2 objects put` takes at most 300 MB**, which 4 MiB chunks stay far below. There is no
  `--cache-control` option on it, which is why caching headers are set by the Worker, not stored.
- **`cf --local` and parallel puts** worked with 8 processes against the local simulation without errors.
- **Index size** grows with the table of contents (4.47 MiB for all of `db/`, about 1 MiB compressed), see above.
- **Bucket location.** The bucket was placed in `WEUR` (nearest the caller when it was created). Reads from
  elsewhere cross to it on a cache miss; the edge cache absorbs repeats.
- **Static assets are capped at 25 MiB each and 20,000 files** on the Free plan (Workers limits page); the
  page and its scripts are far below. The wasm binary goes in R2, not in assets.

## Open item: `cf-cache-status` shows `n/a`

What the owner saw: on the deployed origin every chunk row of the hosting check page reads
`cf-cache-status n/a`, next to `cache-control public, max-age=31536000, immutable`. So whether the edge
cache serves repeated reads is not confirmed. My best explanation, from the code and the Cloudflare docs
read on 2026-11-10 (not tested on the deployed origin, which sits behind Access):

- **The page is not at fault.** [`app.js`](../../web/worker/public/app.js) line 116 prints
  `r.headers.get("cf-cache-status") || "n/a"` for a same-origin response, so `n/a` means the header was not
  on the response; the display is not hiding it.
- **The Worker never sets it, and nothing in front of the Worker does.** The Worker's cache is the Cache
  API (`caches.default`, [`worker.js`](../../web/worker/src/worker.js) lines 79 to 85 for the lookup and 119
  to 122 for the store). The docs describe `Cf-Cache-Status` ("every response carries" it) under Workers
  Cache, a separate feature that is switched on with `"cache": { "enabled": true }` in the Wrangler config;
  `wrangler.example.jsonc` has no `cache` setting, so it is off. I found no statement that a Cache API hit or
  miss adds the header. `withIsolation` (lines 29 to 33) copies all headers of the inner response, so it
  would not remove one. Hence `n/a` is expected on a hit as well as on a miss, and says nothing about
  whether the cache works.
- **Also unknown:** whether the owner's load was warm at all (a first load after a publish is a miss; the
  store runs in `waitUntil`).

Not a page-only display bug, so nothing is changed here and the Worker is not redeployed. For W1:

1. Make hits observable whichever cache is used: add a header such as `X-Anomaly-Cache: hit|miss` in the
   Worker (hit is the `cache.match` branch) and show it on the page, then reload on the deployed origin and
   compare the first and second load.
2. Or move to Workers Cache (`"cache": { "enabled": true }`). The docs say Cloudflare then answers hits
   before the Worker runs, which also cuts Worker requests (relevant to the 100,000 a day limit above; I did
   not check whether a hit still counts as a request), sets `Cf-Cache-Status` on every response, and serves
   `Range` itself from a cached full `200`: it strips `Range` before the Worker runs, and a `206` returned by
   the Worker is not stored. That would change the Worker's own `Range` and 416 handling and its tests. I did
   not try it; check that it is available on the account's plan and not a beta first.

## Not verified

- Edge-cache behaviour on the deployed Worker and compression of `index.json` at the edge: see the open item
  above; neither can be seen without a session.
- Owner-check steps 5 and 6: that a private window gets the login page again, and that a second account not
  on the list is refused. Not reported.
- The tear-down commands and the invite steps; only the `--dry-run` output or help text was read. That deleting the Worker also removes the DNS record it owns is the documented behaviour of a custom domain, not something I saw.
- Worker request counts for a full download of the 16 GB, and the Workers plan of the account.
- Serving this volume through Cloudflare under the account's terms of service. R2 and Workers are the
  products Cloudflare provides for it, but I did not read the terms.
- `cf dev` as an alternative to `wrangler dev`.

## Proposed changes to the plan

- Hosting section: the Worker is deployed with `wrangler`, not `cf`, until `cf deploy` reads
  `wrangler.jsonc` or `cloudflare.config.ts` gains assets settings; the other Cloudflare work stays on `cf`.
- Files and game data: the index carries each archive's decoded table of contents (names are byte strings),
  so the web file backend needs no request to register an archive. The measured size for all of `db/` is
  4.47 MiB (about 1 MiB compressed); decide at W1 whether to split it per archive.
- The first full publish of `db/` took 31.5 minutes at 8.6 MiB/s with 8 `cf` processes; a data update that
  changes few chunks is quick. The cache hit check (open item above) goes to W1.
- The numbers in the plan's hosting cost line hold: about $0.10 a month for storage, free egress; request
  volume on the Workers plan, not R2, is the thing to watch.
