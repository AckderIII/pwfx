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

/// A response curve laid out for drawing: SVG path data in a fixed viewbox
/// -- `CURVE_W` wide, `CURVE_FREQ_H` or `CURVE_W` tall -- and the figures for
/// the axes. Plain data, so the window can compare it and leave an unchanged
/// plot alone.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CurveDraw {
    pub level: bool,
    /// The response itself.
    pub path: String,
    /// The grid: decades and octave marks, and every 6 or 12 dB.
    pub grid: String,
    /// 0 dB on a frequency plot; the 1:1 line on a level plot.
    pub axis: String,
    /// The dB at the top and bottom edges of a frequency plot, and where
    /// 0 dB falls between them, as a fraction from the top.
    pub top: String,
    pub bottom: String,
    pub zero: f32,
}

pub const CURVE_W: f64 = 1000.0;
pub const CURVE_FREQ_H: f64 = 240.0;

/// The span of a level plot, the same on both axes: the filters' grid runs
/// -90 to +6, and a square plot with equal scales is what lets a ratio be
/// read off as a slope.
pub const LEVEL_LO: f64 = -90.0;
pub const LEVEL_HI: f64 = 6.0;

pub fn curve_draw(curve: &crate::engine::Curve) -> CurveDraw {
    if curve.level {
        level_draw(&curve.points)
    } else {
        freq_draw(&curve.points)
    }
}

fn freq_x(f: f64) -> f64 {
    CURVE_W * (f.max(20.0) / 20.0).log10() / 3.0
}

fn freq_draw(points: &[(f64, f64)]) -> CurveDraw {
    // Enough room to show the shape, in 6 dB steps, with 0 dB always on
    // the plot. A steep filter's skirt runs off the bottom at -60 rather
    // than squashing everything above it into a line.
    let hi = points.iter().map(|p| p.1).fold(0.0, f64::max);
    let lo = points.iter().map(|p| p.1).fold(0.0, f64::min);
    let top = (((hi + 3.0) / 6.0).ceil() * 6.0).clamp(6.0, 30.0);
    let bottom = (((lo - 3.0) / 6.0).floor() * 6.0).clamp(-60.0, -6.0);
    let y = |db: f64| CURVE_FREQ_H * (top - db.clamp(bottom, top)) / (top - bottom);

    let mut grid = String::new();
    for decade in [20.0, 200.0, 2000.0] {
        for m in 1..=9 {
            let f = decade * m as f64 / 2.0;
            if (20.0..=20000.0).contains(&f) {
                let x = freq_x(f);
                grid += &format!("M {x:.1} 0 L {x:.1} {CURVE_FREQ_H} ");
            }
        }
    }
    let step = if top - bottom > 48.0 { 12.0 } else { 6.0 };
    let mut db = (bottom / step).ceil() * step;
    while db <= top {
        if db != 0.0 {
            let y = y(db);
            grid += &format!("M 0 {y:.1} L {CURVE_W} {y:.1} ");
        }
        db += step;
    }

    CurveDraw {
        level: false,
        path: polyline(points.iter().map(|&(f, db)| (freq_x(f), y(db)))),
        grid,
        axis: format!("M 0 {0:.1} L {CURVE_W} {0:.1}", y(0.0)),
        top: format!("{top:+.0}"),
        bottom: format!("{bottom:.0}"),
        zero: (top / (top - bottom)) as f32,
    }
}

fn level_draw(points: &[(f64, f64)]) -> CurveDraw {
    let span = LEVEL_HI - LEVEL_LO;
    let x = |db: f64| CURVE_W * (db.clamp(LEVEL_LO, LEVEL_HI) - LEVEL_LO) / span;
    let y = |db: f64| CURVE_W - x(db);

    let mut grid = String::new();
    let mut db = -84.0;
    while db <= LEVEL_HI {
        grid += &format!("M {0:.1} 0 L {0:.1} {CURVE_W} M 0 {1:.1} L {CURVE_W} {1:.1} ", x(db), y(db));
        db += 12.0;
    }

    CurveDraw {
        level: true,
        path: polyline(points.iter().map(|&(i, o)| (x(i), y(o)))),
        grid,
        axis: format!("M 0 {CURVE_W} L {CURVE_W} 0"),
        ..Default::default()
    }
}

fn polyline(mut pts: impl Iterator<Item = (f64, f64)>) -> String {
    let Some((x, y)) = pts.next() else {
        return String::new();
    };
    let mut s = format!("M {x:.1} {y:.1}");
    for (x, y) in pts {
        s += &format!(" L {x:.1} {y:.1}");
    }
    s
}

/// What a chain's header says about the latency its filters add.
pub fn chain_latency_text(ms: Option<f64>) -> String {
    match ms {
        None => String::new(),
        Some(ms) if ms < 0.05 => "no added latency".into(),
        Some(ms) => format!("+{ms:.1} ms latency"),
    }
}

/// The same, as the last part of a chain's line in the sidebar; nothing
/// when there is nothing to say.
pub fn chain_latency_short(ms: Option<f64>) -> Option<String> {
    ms.filter(|ms| *ms >= 0.05).map(|ms| format!("{ms:.1} ms"))
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
    fn a_chains_latency_reads_as_what_it_adds() {
        assert_eq!(chain_latency_text(None), "");
        assert_eq!(chain_latency_text(Some(0.0)), "no added latency");
        assert_eq!(chain_latency_text(Some(5.25)), "+5.2 ms latency");
        assert_eq!(chain_latency_short(Some(0.0)), None);
        assert_eq!(chain_latency_short(Some(10.0)).as_deref(), Some("10.0 ms"));
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

    use crate::engine::Curve;

    fn nums(path: &str) -> Vec<f64> {
        path.split_whitespace().filter_map(|t| t.parse().ok()).collect()
    }

    #[test]
    fn a_frequency_curve_fits_its_box_and_keeps_0_db_on_it() {
        // A flat EQ: the smallest range, 0 dB in the middle.
        let flat = Curve::parse("freq 20:0,1000:0,20000:0").unwrap();
        let d = curve_draw(&flat);
        assert_eq!((d.top.as_str(), d.bottom.as_str()), ("+6", "-6"));
        assert!((d.zero - 0.5).abs() < 1e-6);
        assert_eq!(d.path, "M 0.0 120.0 L 566.3 120.0 L 1000.0 120.0");

        // A steep low-pass: clamped at -60 rather than squashing the passband.
        let lp = Curve::parse("freq 20:0,1000:-3,20000:-210").unwrap();
        let d = curve_draw(&lp);
        assert_eq!(d.bottom, "-60");
        assert!(nums(&d.path).iter().all(|v| (0.0..=CURVE_W).contains(v)));
        assert!(nums(&d.path).iter().skip(1).step_by(2).all(|y| *y <= CURVE_FREQ_H));

        // A +9 dB bell gets room above it.
        let bell = Curve::parse("freq 20:0,1000:9,20000:0").unwrap();
        assert_eq!(curve_draw(&bell).top, "+12");
    }

    #[test]
    fn a_level_curve_is_square_and_keeps_its_order() {
        // A gate's loop: the path must come back on itself, in order.
        let gate = Curve::parse("level -90:-110,-40:-60,-40:-40,6:6,-46:-46,-46:-66").unwrap();
        let d = curve_draw(&gate);
        assert!(d.level);
        let n = nums(&d.path);
        let xs: Vec<f64> = n.iter().step_by(2).copied().collect();
        assert_eq!(xs.len(), 6);
        assert!(xs[3] > xs[4], "the path turns back at the top");
        // 1:1 runs corner to corner, so equal scales show a ratio as a slope.
        assert_eq!(d.axis, "M 0 1000 L 1000 0");
        // -90 in maps to the left edge, and -110 out is clamped to the bottom.
        assert_eq!((n[0], n[1]), (0.0, CURVE_W));
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
