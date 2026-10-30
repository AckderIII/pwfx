//! Turning engine state into the models the window displays, and drags back
//! into parameter values.
//!
//! Kept apart from `main.rs` so the arithmetic that has to agree with itself
//! -- a slider position and the value it means -- sits in one place and can be
//! tested without a window.

use crate::plugin::{ParamDesc, ParamKind};

/// Where on a 0..1 slider a value sits.
pub fn to_pos(p: &ParamDesc, value: f64) -> f32 {
    let ParamKind::Number { min, max, .. } = p.kind else {
        return 0.0;
    };
    if max <= min {
        return 0.0;
    }
    let v = value.clamp(min, max);
    let pos = if p.logarithmic() {
        let floor = p.log_floor(min);
        if v <= floor {
            0.0
        } else {
            (v / floor).ln() / (max / floor).ln()
        }
    } else {
        (v - min) / (max - min)
    };
    pos.clamp(0.0, 1.0) as f32
}

/// And back again. The inverse of `to_pos` everywhere it can be, so that
/// letting go of a slider and picking it up again does not move it.
pub fn from_pos(p: &ParamDesc, pos: f32) -> f64 {
    let ParamKind::Number { min, max, integer } = p.kind else {
        return 0.0;
    };
    let pos = (pos as f64).clamp(0.0, 1.0);
    let v = if p.logarithmic() {
        let floor = p.log_floor(min);
        if pos <= 0.0 {
            min
        } else {
            floor * (max / floor).powf(pos)
        }
    } else {
        min + pos * (max - min)
    };
    let v = if integer { v.round() } else { round_for_display(v) };
    v.clamp(min, max)
}

/// A slider drag produces a full-precision double; nobody wants a cutoff of
/// 2384.7719116210938 Hz. Round to something a person would have typed, at a
/// resolution fine enough that the slider still feels continuous.
fn round_for_display(v: f64) -> f64 {
    let mag = v.abs();
    let step = if mag >= 1000.0 {
        1.0
    } else if mag >= 100.0 {
        0.1
    } else if mag >= 10.0 {
        0.01
    } else {
        0.001
    };
    (v / step).round() * step
}

/// `%g` with six significant digits, which is how the filters print numbers.
/// Matching them keeps a value the GUI produced from looking different to the
/// same value echoed back.
pub fn fmt_g(v: f64) -> String {
    if !v.is_finite() {
        return "0".into();
    }
    if v == v.trunc() && v.abs() < 1e15 {
        return format!("{}", v as i64);
    }
    let exp = v.abs().log10().floor() as i32;
    let decimals = (5 - exp).clamp(0, 12) as usize;
    let s = format!("{v:.decimals$}");
    let s = if s.contains('.') {
        s.trim_end_matches('0').trim_end_matches('.').to_string()
    } else {
        s
    };
    if s == "-0" {
        "0".into()
    } else {
        s
    }
}

/// The text shown next to a slider, taken from what the filter reported when
/// that is a number and left alone when it is not.
pub fn display_value(raw: &str) -> String {
    match raw.parse::<f64>() {
        Ok(v) => fmt_g(v),
        Err(_) => raw.to_string(),
    }
}

/// The figure printed beside a level meter, in dBFS.
///
/// The filters floor at -240 dB and report that when a meter is armed over
/// silence, so anything down there is shown as nothing rather than as a
/// number: -240.0 is not a reading, it is the absence of one. Everything a
/// signal can actually reach gets one decimal, which is as fine as a peak
/// hold that falls back at 20 dB/s is worth reading.
pub fn level_text(db: f32) -> String {
    if !db.is_finite() || db <= LEVEL_FLOOR {
        "\u{2014}".into() // an em dash
    } else {
        format!("{db:.1}")
    }
}

/// The figure printed beside a gain-reduction meter.
///
/// Negative, the way a compressor's meter is marked: it is how far the level
/// is being pulled down. No reduction is a reading -- the filter is there and
/// doing nothing to this signal -- so it is printed as one, not as a dash.
pub fn gr_text(db: f32) -> String {
    if !db.is_finite() || db < 0.05 {
        "0.0".into()
    } else {
        format!("-{db:.1}")
    }
}

/// Below this a reading is treated as silence. Well under anything audible,
/// and well above the filters' own -240 floor, so a meter decaying after a
/// signal stops still shows figures on the way down.
const LEVEL_FLOOR: f32 = -120.0;

/// How a chain of this width is described in the width picker.
pub fn width_label(n: usize) -> String {
    match n {
        1 => "Mono (1 channel)".into(),
        2 => "Stereo (2 channels)".into(),
        _ => format!("{n} channels"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::plugin::parse_help;

    fn param(range: &str, placeholder: &str) -> ParamDesc {
        let help = format!(
            "pw_x -- x\n\nfilter parameters (live, settable on stdin)\n  -a, --knob {placeholder}             a knob\n                             ({range})\n\nstream and node\n\ncontrol protocol\n"
        );
        parse_help(&help).unwrap().param("knob").unwrap().clone()
    }

    #[test]
    fn a_slider_round_trips() {
        let p = param("-120..0 dB, default -35", "DB");
        for pos in [0.0f32, 0.1, 0.25, 0.5, 0.75, 1.0] {
            let v = from_pos(&p, pos);
            assert!((to_pos(&p, v) - pos).abs() < 1e-3, "{pos} -> {v}");
        }
    }

    #[test]
    fn a_frequency_slider_is_logarithmic_and_round_trips() {
        let p = param("1..384000 Hz, default 12000", "HZ");
        assert!(p.logarithmic());
        // 1 kHz on a 1 Hz..384 kHz range: a third of the way up on a log
        // scale, a quarter of a percent on a linear one.
        let pos = to_pos(&p, 1000.0);
        assert!((0.50..0.56).contains(&pos), "1 kHz sat at {pos}");
        for v in [1.0, 20.0, 1000.0, 20000.0, 384000.0] {
            let back = from_pos(&p, to_pos(&p, v));
            assert!((back - v).abs() / v < 1e-3, "{v} -> {back}");
        }
    }

    #[test]
    fn zero_hertz_means_off_and_is_reachable() {
        let p = param("0..384000 Hz, default 0", "HZ");
        assert!(p.logarithmic());
        assert_eq!(to_pos(&p, 0.0), 0.0);
        assert_eq!(from_pos(&p, 0.0), 0.0);
        // Just off the bottom is 1 Hz, not 0.
        assert!(from_pos(&p, 0.001) >= 1.0);
    }

    #[test]
    fn an_integer_parameter_never_lands_between_two_values() {
        let p = param("1..16, default 4", "N");
        for i in 0..=40 {
            let v = from_pos(&p, i as f32 / 40.0);
            assert_eq!(v, v.round(), "{v}");
            assert!((1.0..=16.0).contains(&v));
        }
        assert_eq!(from_pos(&p, 0.0), 1.0);
        assert_eq!(from_pos(&p, 1.0), 16.0);
    }

    #[test]
    fn dragged_values_are_not_absurdly_precise() {
        let p = param("-80..0 dB, default -18", "DB");
        for i in 0..=100 {
            let s = fmt_g(from_pos(&p, i as f32 / 100.0));
            let digits = s.split('.').nth(1).map(|d| d.len()).unwrap_or(0);
            assert!(digits <= 2, "{s} has {digits} decimals");
        }
    }

    #[test]
    fn a_level_reads_as_a_number_or_as_nothing() {
        assert_eq!(level_text(-12.34), "-12.3");
        assert_eq!(level_text(0.0), "0.0");
        assert_eq!(level_text(-5.0), "-5.0");
        // Over: still a number, and the colour beside it does the shouting.
        assert_eq!(level_text(0.4), "0.4");
        // The filters' floor, and the decay towards it.
        assert_eq!(level_text(-240.0), "\u{2014}");
        assert_eq!(level_text(-119.9), "-119.9");
        assert_eq!(level_text(-120.0), "\u{2014}");
        assert_eq!(level_text(f32::NEG_INFINITY), "\u{2014}");

        // Every reading a signal can reach fits the column it is printed in.
        for i in 0..=1200 {
            let db = -120.0 + i as f32 * 0.1;
            assert!(level_text(db).len() <= 6, "{db} -> {}", level_text(db));
        }
    }

    #[test]
    fn gain_reduction_reads_as_a_cut() {
        assert_eq!(gr_text(14.66), "-14.7");
        assert_eq!(gr_text(0.0), "0.0");
        // Under what the filter's own rounding can report: no reduction,
        // not a "-0.0".
        assert_eq!(gr_text(0.04), "0.0");
        assert_eq!(gr_text(f32::NAN), "0.0");
        // A gate snapped shut at its range, in the same column.
        assert!(gr_text(120.0).len() <= 6);
    }

    #[test]
    fn numbers_print_the_way_the_filters_print_them() {
        assert_eq!(fmt_g(80.0), "80");
        assert_eq!(fmt_g(-1.0), "-1");
        assert_eq!(fmt_g(0.5), "0.5");
        assert_eq!(fmt_g(2500.25), "2500.25");
        assert_eq!(fmt_g(-0.0), "0");
        assert_eq!(display_value("orfanidis"), "orfanidis");
        assert_eq!(display_value("-35.000"), "-35");
    }
}
