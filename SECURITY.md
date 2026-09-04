# Security Policy

First, some context so you know what you are dealing with: Sonero is a hobby
project. One person builds it in spare time and puts it out publicly because it
might be useful to someone else — there is no company behind it, no security
team, and no on-call anyone. I take security reports seriously and will fix what
I can, but please read the rest of this page as a promise made by a person, not
by an organisation.

That said, Sonero is not a toy either. It runs as your normal user, with no
setuid binary and nothing running as root, but it does touch a few things worth
reporting if they can be abused:

- PipeWire and WirePlumber configuration it writes into `~/.config/pipewire/`
  and `~/.config/wireplumber/`.
- `/dev/hidraw*` access to SteelSeries devices, through the udev rule the `.deb`
  installs (`uaccess`, so the active local user only).
- BlueZ over D-Bus, for Bluetooth codecs and battery levels.
- Its own settings and equalizer presets read from disk — including a preset
  file someone imported from somewhere untrusted.
- The autostart entry it writes into `~/.config/autostart`.

## Supported Versions

Fixes go into the next release. There are no backports to older ones — with one
maintainer, keeping several branches alive is not realistic.

| Version                | Supported          |
| ---------------------- | ------------------ |
| Latest release (0.1.x) | :white_check_mark: |
| Older 0.1.x releases   | :x:                |
| `dev` prerelease       | :x: — rebuilt from `main` on every push, so it is not a release |
| < 0.1                  | :x:                |

Please check that the problem still happens on the latest
[release](https://github.com/Tatsu21/sonero/releases) before reporting it.

## Reporting a Vulnerability

Please don't open a public issue for a security problem. Use GitHub's private
reporting instead:
[**Report a vulnerability**](https://github.com/Tatsu21/sonero/security/advisories/new)
(the Security tab → Advisories). Only I can read it. That is the only channel —
there is no security mailbox to write to.

Useful to include, as much of it as you have:

- Sonero version and how you installed it (`.deb`, AppImage, from source).
- Your distribution, Wayland or X11, and `pipewire --version`.
- What an attacker gets out of it, and what they need to start with — a local
  account, another app in the same session, a crafted preset file, a Bluetooth
  device in range.
- How to reproduce it, plus a log or backtrace if you have one.

## What happens next

I'll reply as soon as I see the report — usually within a few days, but it may
take longer if life gets in the way. If a week or two goes by with nothing, feel
free to ping the advisory thread; the report was almost certainly missed, not
ignored.

If it turns out to be real, I'll fix it and ship it in the next release, then
publish an advisory. I'll credit you however you like, or not at all if you
prefer. If I don't think it's an issue, I'll say why rather than let the report
go quiet — usually because the behaviour is intended, because it needs access
that already means the account is lost, or because it belongs to PipeWire,
BlueZ, Qt or the kernel rather than to Sonero. In that last case I'll point you
at where it does belong.

As for going public: please give me a reasonable head start, say around 90 days
or until there's a fixed release, whichever comes first. If I'm clearly dragging
past that, say so on the thread and we'll agree on a date — I'd rather that than
have a real problem sit unfixed and unmentioned.

## Not really in scope

- Bugs in PipeWire, WirePlumber, BlueZ, Qt or your distribution's packages —
  those projects want to hear about them, not me.
- Anything that needs the attacker to already have your account or root.
- Scanner output with no demonstrated effect on Sonero.
- The `dev` prerelease, unless it also happens on a release.

Ordinary bugs, crashes and anything else that isn't security-sensitive are very
welcome as normal [issues](https://github.com/Tatsu21/sonero/issues) — no need
for the private channel.
