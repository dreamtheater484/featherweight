---
title: Privacy
description: Featherweight collects nothing. Its only network access is the update check, which can be turned off.
---

# Privacy

**No telemetry, no account, no subscription; the only network access is the update check, which can be turned off.**

Featherweight doesn't collect, store or send any personal data, and it doesn't send your documents anywhere. There are
no ads, no analytics and no background services.

## The update check

- At most once a day, Featherweight asks GitHub (`api.github.com`) whether there's a newer release. The request carries
  Featherweight's version, as every program's request carries its name; nothing about you or your files.
- When there's a new release, Featherweight downloads it from the project's GitHub releases and checks it against the
  release's SHA-256 sums before it installs it.
- Like any website, GitHub sees your IP address when Featherweight asks. GitHub's
  [privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)
  covers that.
- Turn it off with **⋯** → **Update automatically**. Then Featherweight makes no network requests at all, unless you
  choose **Check for updates now**.

Opening, editing, saving or printing a document never makes a network request. Pictures and other content a document
links to on the web aren't fetched. When you paste from a web page, its pictures are downloaded only if you click the
button for it.

## What stays on your PC

All of it, in `%LOCALAPPDATA%\Featherweight` and `%APPDATA%\Featherweight`, or beside a portable copy:
- **Settings,** such as the window's place and your recent files.
- **Backups:** the original of a document Featherweight saved over, for 30 days.
- **Recovery copies** of unsaved changes, encrypted for your Windows account, removed when you save or close.
- **Crash reports:** if Featherweight crashes, a small report of where it was at that moment: its threads and their
  stacks (the 5 newest are kept). Nothing is sent: the next start offers to open a GitHub issue in your browser, and you decide whether to attach the report.

Uninstalling Featherweight leaves these folders; delete them to remove everything.

## Links you click

Help, Known issues, Report a problem, and web links in documents open in your browser only when you click them. What
happens there is between you and that website. GitHub issues are public.

## Contact

Questions about privacy: [open an issue]({{ site.repo }}/issues/new) on GitHub.
