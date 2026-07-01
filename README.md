# obs-censor

English | [Русский](README.ru-RU.md)

`obs-censor` is an OBS Studio filter plugin that automatically hides or shows the source it is attached to based on MPC-BE playback time.

> Note: this plugin was created with AI assistance.

This plugin works as a **source filter**:

- you attach it directly to the source you want to control;
- it polls MPC-BE's `variables.html` endpoint;
- it checks the current playback time against configured timing intervals;
- it **shows** the source during matching intervals and **hides** it outside them.

## Features

- OBS source filter for timing-based source visibility control
- MPC-BE playback sync through HTTP (`variables.html`)
- Multiple timing ranges in `HH:MM:SS-HH:MM:SS` format
- Automatic correction of reversed ranges such as `01:28:19-01:26:21`
- Signed timing offset support in `+/-HH:MM:SS` format
- English and Russian localization (`en-US`, `ru-RU`)
- Windows support via WinINet and Linux support via libcurl

## How it works

The filter runs a background worker that periodically requests the MPC-BE status page.

It reads playback position from either:

- `<p id="positionstring">HH:MM:SS</p>`, or
- `<p id="position">milliseconds</p>`

Then it:

1. parses your configured timing intervals;
2. applies the optional timing offset;
3. checks whether the adjusted playback time is inside any interval;
4. renders the source only when the time matches.

### Visibility logic

- **Inside** a configured interval -> source is visible
- **Outside** all configured intervals -> source is hidden
- **No URL or no timings configured** -> hiding is disabled and the source stays visible

> Note: this plugin currently controls **video visibility**. It does not mute or censor audio.

## OBS usage

1. In MPC-BE, enable the web interface and make sure `variables.html` is reachable.
   - Default URL: `http://localhost:13579/variables.html`
2. In OBS Studio, open the source you want to control.
3. Add the **MPC-BE Censor** filter to that source.
4. Configure the filter settings.

## Filter settings

### MPC-BE URL

The HTTP endpoint used to fetch playback position.

Default:

```text
http://localhost:13579/variables.html
```

### Poll interval (ms)

How often the filter checks MPC-BE.

- Minimum: `500`
- Maximum: `5000`
- Default: `1000`

### Timing offset

Shifts all timings without editing the timing list itself.

Accepted formats:

- `+00:00:05`
- `-00:00:05`
- `00:01:30`
- plain seconds such as `15` or `-15` are also accepted

Behavior:

- `+` delays the trigger
- `-` advances the trigger

Examples:

- `+00:00:05` -> the source changes state 5 seconds later
- `-00:00:05` -> the source changes state 5 seconds earlier

### Timings

Enter one or more time ranges in this format:

```text
00:03:45-00:03:49
00:10:00-00:10:12
01:28:19-01:26:21
```

The filter searches the text for timing ranges, so notes after a range are fine:

```text
00:03:45-00:03:49 - short censor
00:10:00-00:10:12 - second scene
```

If a range is entered backwards, it is fixed automatically.

## Build

Windows build commands:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

The main output is:

```text
build_x64/RelWithDebInfo/obs-censor.dll
```

The runnable local plugin layout created by the build is under:

```text
build_x64/rundir/RelWithDebInfo/
```

## Installation notes

If you install the plugin manually, do **not** copy only the DLL.

You also need the plugin data files, especially the locale files:

```text
obs-censor/locale/en-US.ini
obs-censor/locale/ru-RU.ini
```

If the locale files are missing, OBS may show raw keys such as:

```text
MpcBeCensorFilter.Name
MpcBeCensorFilter.Timings
```

For local testing, use the files produced in:

```text
build_x64/rundir/RelWithDebInfo/
```

## Localization

Currently included:

- English: `data/locale/en-US.ini`
- Russian: `data/locale/ru-RU.ini`

## Project info

- Plugin name: `obs-censor`
- OBS filter id: `mpc_be_censor_filter`
- Display name: `OBS Censor Plugin`
- Website: <https://github.com/NowaLone/obs-censor>

## License

This project is licensed under GPL v2 or later, matching the plugin source headers.
