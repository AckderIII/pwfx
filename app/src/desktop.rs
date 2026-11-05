//! `pwfx --install-desktop`: put the desktop file and the icon where the
//! desktop looks for them.
//!
//! The window's app id names `pwfx.desktop`, and that is how GNOME and the
//! others find the window's name and icon -- but only a desktop file in one of
//! the XDG data directories counts, and the one inside an AppImage is not in
//! any of them. So this writes both into `$XDG_DATA_HOME`, with `Exec=`
//! pointing at whatever is running now: the AppImage itself when there is one
//! (`$APPIMAGE`, set by its runtime -- the executable would be a path inside a
//! mount that is gone once it exits), otherwise this binary.
//!
//! Only ever on request. Run it again after moving the AppImage; delete the
//! two files it names to undo it.

use std::path::{Path, PathBuf};

const DESKTOP: &str = include_str!("../../packaging/appimage/pwfx.desktop");
const ICON: &str = include_str!("../../packaging/appimage/pwfx.svg");

pub fn install() -> Result<Vec<PathBuf>, String> {
    let exe = match std::env::var_os("APPIMAGE") {
        Some(p) if !p.is_empty() => PathBuf::from(p),
        _ => std::env::current_exe()
            .map_err(|e| format!("cannot tell where pwfx is: {e}"))?,
    };
    let exe = exe.to_str()
        .ok_or_else(|| format!("{}: not a UTF-8 path, which a desktop file cannot hold", exe.display()))?;
    let data = data_home()?;
    let entry = data.join("applications/pwfx.desktop");
    let icon = data.join("icons/hicolor/scalable/apps/pwfx.svg");
    write(&entry, &desktop_entry(DESKTOP, exe))?;
    write(&icon, ICON)?;
    Ok(vec![entry, icon])
}

fn data_home() -> Result<PathBuf, String> {
    // The spec says a relative $XDG_DATA_HOME is invalid and to be ignored.
    if let Some(d) = std::env::var_os("XDG_DATA_HOME").map(PathBuf::from) {
        if d.is_absolute() {
            return Ok(d);
        }
    }
    match std::env::var_os("HOME") {
        Some(h) if !h.is_empty() => Ok(PathBuf::from(h).join(".local/share")),
        _ => Err("neither XDG_DATA_HOME nor HOME is set".into()),
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
}
