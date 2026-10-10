# winget

Featherweight is published to the Windows Package Manager as `dreamtheater484.Featherweight`:

```bash
winget install dreamtheater484.Featherweight
```

`0.9.3/` holds the manifests of the first submission
([microsoft/winget-pkgs#450079](https://github.com/microsoft/winget-pkgs/pull/450079)).
Once that's accepted, later versions are submitted by the release script:

```bash
powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Publish -Winget
```

This needs `wingetcreate` (`winget install Microsoft.WingetCreate`) and the GitHub CLI logged in.
Microsoft's bots validate each submission (virus scan, test install) before it goes live.
