//! `pwfx --install-desktop` and `pwfx --install-service`: put the files that
//! start pwfx where the desktop and systemd look for them, pointing at
//! whatever is running now -- the AppImage itself when there is one
//! (`$APPIMAGE`, set by its runtime: the executable would be a path inside a
//! mount that is gone once it exits), otherwise this binary.
//!
//! Only ever on request, and only files: nothing is enabled or started. Run
//! either again after moving the AppImage; delete the files it names to undo
//! it.
//!
//! The window's app id names `pwfx.desktop`, and that is how GNOME and the
//! others find the window's name and icon -- but only a desktop file in one of
//! the XDG data directories counts, and the one inside an AppImage is not in
//! any of them. So `--install-desktop` writes both into `$XDG_DATA_HOME`.
//!
//! `--install-service` writes the user unit `pwfx.service` into
//! `$XDG_CONFIG_HOME/systemd/user`. It leaves enabling it to the user: only one
//! pwfx runs at a time, so a service started behind their back would refuse
//! the window they open next.

use std::path::{Path, PathBuf};

const DESKTOP: &str = include_str!("../../packaging/appimage/pwfx.desktop");
const ICON: &str = include_str!("../../packaging/appimage/pwfx.svg");
const SERVICE: &str = include_str!("../pwfx.service");

pub fn desktop() -> Result<Vec<PathBuf>, String> {
    let exe = executable()?;
    let data = xdg_home("XDG_DATA_HOME", ".local/share")?;
    let entry = data.join("applications/pwfx.desktop");
    let icon = data.join("icons/hicolor/scalable/apps/pwfx.svg");
    write(&entry, &desktop_entry(DESKTOP, &exe))?;
    write(&icon, ICON)?;
    Ok(vec![entry, icon])
}

pub fn service() -> Result<Vec<PathBuf>, String> {
    let exe = executable()?;
    let unit = xdg_home("XDG_CONFIG_HOME", ".config")?.join("systemd/user/pwfx.service");
    write(&unit, &service_unit(SERVICE, &exe))?;
    Ok(vec![unit])
}

/// What the written files are to start, as a string: both formats are text,
/// and neither can hold a path that is not UTF-8.
fn executable() -> Result<String, String> {
    let exe = match std::env::var_os("APPIMAGE") {
        Some(p) if !p.is_empty() => PathBuf::from(p),
        _ => std::env::current_exe()
            .map_err(|e| format!("cannot tell where pwfx is: {e}"))?,
    };
    exe.to_str().map(str::to_owned)
        .ok_or_else(|| format!("{}: not a UTF-8 path, which these files cannot hold", exe.display()))
}

/// `$var`, or `~/fallback` without it. The spec says a relative value is
/// invalid and to be ignored.
fn xdg_home(var: &str, fallback: &str) -> Result<PathBuf, String> {
    if let Some(d) = std::env::var_os(var).map(PathBuf::from) {
        if d.is_absolute() {
            return Ok(d);
        }
    }
    match std::env::var_os("HOME") {
        Some(h) if !h.is_empty() => Ok(PathBuf::from(h).join(fallback)),
        _ => Err(format!("neither {var} nor HOME is set")),
    }
}

/// Written beside the target and renamed over it, so a desktop scanning the
/// directory never reads half a file.
fn write(path: &Path, contents: &str) -> Result<(), String> {
    let err = |e: std::io::Error| format!("{}: {e}", path.display());
    std::fs::create_dir_all(path.parent().expect("an absolute path")).map_err(err)?;
    let tmp = path.with_extension("tmp");
    std::fs::write(&tmp, contents).map_err(err)?;
    std::fs::rename(&tmp, path).map_err(err)
}

/// The template with `Exec=` and `TryExec=` naming `exe`. `TryExec` makes the
/// desktop hide the entry once the file is gone, instead of offering a
/// launcher that does nothing.
fn desktop_entry(template: &str, exe: &str) -> String {
    let mut out = String::new();
    for line in template.lines() {
        if line.starts_with("Exec=") {
            out += &format!("Exec={}\n", exec_arg(exe));
            out += &format!("TryExec={}\n", escape(exe));
        } else if !line.starts_with("TryExec=") {
            out += line;
            out += "\n";
        }
    }
    out
}

/// A string value as the desktop entry spec reads it: `\` escaped first.
fn escape(s: &str) -> String {
    s.replace('\\', "\\\\")
}

/// One argument of `Exec=`. Quoted if it holds anything the spec reserves,
/// with `"`, `` ` ``, `$` and `\` backslashed inside the quotes -- and then
/// escaped again as a string value, which is why a backslash ends up four.
/// `%` is a field code everywhere and is doubled.
fn exec_arg(s: &str) -> String {
    let s = s.replace('%', "%%");
    let reserved = |c: char| " \t\n\"'\\><~|&;$*?#()`".contains(c);
    if !s.contains(reserved) {
        return s;
    }
    let mut q = String::from("\"");
    for c in s.chars() {
        if matches!(c, '"' | '`' | '$' | '\\') {
            q.push('\\');
        }
        q.push(c);
    }
    q.push('"');
    escape(&q)
}

/// The unit with `ExecStart=` naming `exe`, and the template's opening
/// comment -- how to install it by hand -- swapped for one saying where this
/// one came from.
fn service_unit(template: &str, exe: &str) -> String {
    let mut out = format!(
        "# Written by `pwfx --install-service`, starting {exe}.\n\
         # Run that again after moving it. To start pwfx from login:\n\
         #\n\
         #   systemctl --user daemon-reload\n\
         #   systemctl --user enable --now pwfx\n\
         #\n\
         # Only one pwfx runs at a time: stop this before opening the window.\n\n");
    let body = template.trim_start_matches(|c| c != '[');
    for line in body.lines() {
        if line.starts_with("ExecStart=") {
            out += &format!("ExecStart={} --headless\n", unit_arg(exe));
        } else {
            out += line;
            out += "\n";
        }
    }
    out
}

/// One word of a systemd command line. `%` starts a specifier and `$` a
/// variable anywhere, so both are doubled; quoted if it holds whitespace,
/// a quote or a backslash, with C escapes inside the quotes.
fn unit_arg(s: &str) -> String {
    let s = s.replace('%', "%%").replace('$', "$$");
    if !s.contains(|c: char| c.is_whitespace() || "\"'\\".contains(c)) {
        return s;
    }
    let mut q = String::from("\"");
    for c in s.chars() {
        match c {
            '"' | '\\' => { q.push('\\'); q.push(c); }
            '\t' => q += "\\t",
            '\n' => q += "\\n",
            _ => q.push(c),
        }
    }
    q.push('"');
    q
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exec_points_at_the_executable_and_the_rest_is_kept() {
        let out = desktop_entry(DESKTOP, "/home/u/Apps/pwfx.AppImage");
        assert!(out.contains("\nExec=/home/u/Apps/pwfx.AppImage\n"), "{out}");
        assert!(out.contains("\nTryExec=/home/u/Apps/pwfx.AppImage\n"), "{out}");
        assert_eq!(out.matches("Exec=").count(), 2, "{out}");
        for line in DESKTOP.lines().filter(|l| !l.starts_with("Exec=")) {
            assert!(out.lines().any(|l| l == line), "lost {line:?}");
        }
    }

    #[test]
    fn the_template_names_what_the_window_calls_itself() {
        // The app id is "pwfx": the file must be pwfx.desktop and say so for X11.
        assert!(DESKTOP.lines().any(|l| l == "StartupWMClass=pwfx"));
        assert!(DESKTOP.lines().any(|l| l == "Icon=pwfx"));
    }

    #[test]
    fn awkward_paths_are_quoted_as_the_spec_says() {
        assert_eq!(exec_arg("/opt/pwfx"), "/opt/pwfx");
        assert_eq!(exec_arg("/My Apps/pwfx"), "\"/My Apps/pwfx\"");
        assert_eq!(exec_arg("/a$b/pwfx"), "\"/a\\\\$b/pwfx\"");
        assert_eq!(exec_arg("/a\\b"), "\"/a\\\\\\\\b\"");
        assert_eq!(exec_arg("/100%/pwfx"), "/100%%/pwfx");
    }

    #[test]
    fn the_unit_starts_the_executable_headless_and_keeps_the_rest() {
        let out = service_unit(SERVICE, "/home/u/Apps/pwfx.AppImage");
        assert!(out.lines().any(|l| l == "ExecStart=/home/u/Apps/pwfx.AppImage --headless"), "{out}");
        assert_eq!(out.matches("ExecStart=").count(), 1, "{out}");
        assert!(!out.contains("ln -s"), "the manual install steps stay out: {out}");
        let body = &SERVICE[SERVICE.find('[').unwrap()..];
        for line in body.lines().filter(|l| !l.starts_with("ExecStart=")) {
            assert!(out.lines().any(|l| l == line), "lost {line:?}");
        }
    }

    #[test]
    fn awkward_paths_are_quoted_as_systemd_reads_them() {
        assert_eq!(unit_arg("/opt/pwfx"), "/opt/pwfx");
        assert_eq!(unit_arg("/My Apps/pwfx"), "\"/My Apps/pwfx\"");
        assert_eq!(unit_arg("/100%/pwfx"), "/100%%/pwfx");
        assert_eq!(unit_arg("/a$b/pwfx"), "/a$$b/pwfx");
        assert_eq!(unit_arg("/a b\\c\"d"), "\"/a b\\\\c\\\"d\"");
    }
}
