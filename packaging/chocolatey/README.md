# Chocolatey

`featherweight.nuspec` and `tools\chocolateyinstall.ps1` are written by `packaging\manifests.ps1` from each release's
hashes (`scripts\release.ps1 -Publish` runs it). The package installs the release's installer per machine, silently
(`/VERYSILENT /ALLUSERS`), and checks its SHA-256. A copy in Program Files doesn't update itself; `choco upgrade
featherweight` does.

Pushing needs the Chocolatey account's API key (Roy's step 3):

```powershell
cd packaging\chocolatey
choco pack
choco push featherweight.<version>.nupkg --source https://push.chocolatey.org/ --api-key <key>
```

Chocolatey's moderators review the first version; later ones go through their automatic checks.
