# Scoop

`featherweight.json` is written by `packaging\manifests.ps1` from each release's hashes (`scripts\release.ps1
-Publish` runs it). It installs the portable zip (x64 and ARM64), keeps `Featherweight.ini`, the backups and the
recovery files across updates, and turns the app's own updater off, since Scoop updates it (`scoop update
featherweight`).

It goes into the Extras bucket by a pull request to https://github.com/ScoopInstaller/Extras adding
`bucket/featherweight.json` (Roy's step 3). After that, Scoop's `checkver` and `autoupdate` pick up new releases
from GitHub by themselves.
