// totk.ini as runtime/cfg.c reads it: "key = value" lines, '#' or ';' start a comment, [sections] only group, keys are
// case-insensitive with an optional TOTK_ prefix, a value may be in double quotes, an empty value counts as unset and a
// repeated key's later line wins. Writing keeps every other line, as runtime/cfg.c cfg_save does: an active line is
// rewritten in place, else the commented "# key = ..." line of totk.ini.example is activated, else the line goes into
// its section (created at the end when the file lacks it).
use std::collections::BTreeMap;

/// The key of a "key = value" line, lower-case and without the prefix, and its value; None for anything else.
fn parse(line: &str) -> Option<(String, String)> {
    let s = line.trim();
    if s.is_empty() || s.starts_with(['#', ';', '[']) {
        return None;
    }
    let (k, v) = s.split_once('=')?;
    let k = k.trim().to_ascii_lowercase();
    let k = k.strip_prefix("totk_").unwrap_or(&k).to_string();
    let v = v.trim();
    let v = if v.len() >= 2 && v.starts_with('"') && v.ends_with('"') { &v[1..v.len() - 1] } else { v };
    (!k.is_empty()).then(|| (k, v.to_string()))
}

/// The key of a commented-out "# key = value" line.
fn commented(line: &str) -> Option<String> {
    let s = line.trim_start();
    let rest = s.strip_prefix('#').or_else(|| s.strip_prefix(';'))?;
    parse(rest).map(|(k, _)| k)
}

fn section_of(line: &str) -> Option<String> {
    let s = line.trim();
    s.strip_prefix('[')?.strip_suffix(']').map(|n| n.trim().to_ascii_lowercase())
}

/// Every key set in the file (empty values left out: they mean the default).
pub fn read(text: &str) -> BTreeMap<String, String> {
    let mut out = BTreeMap::new();
    for line in text.trim_start_matches('\u{feff}').lines() {
        if let Some((k, v)) = parse(line) {
            if v.is_empty() { out.remove(&k); } else { out.insert(k, v); }
        }
    }
    out
}

/// The totk.ini.example section a key belongs to (runtime/cfg.c `sections`).
pub fn section_for(key: &str) -> &'static str {
    match key {
        "fps_limit" | "fps60" | "realtime" | "cloth_realtime" | "highfps_fix" => "frame rate",
        "fullscreen" | "window_size" | "perf_overlay" | "vk_present" | "gpu_index" | "gpu_name" | "output_res"
        | "render_scale" | "upscaler" | "sharpness" => "display",
        "lang" | "locale" | "operation_mode" | "menu_lang" | "lang_mod" => "language",
        "audio" | "audio_prefill_ms" | "volume" => "audio",
        "motion" | "rumble" | "rumble_strength" | "rumble_curve" | "rumble_triggers" | "pad_layout" | "mouse_sens"
        | "input_record" => "input",
        "romfs_mods" | "romfs_patches" => "files",
        "trace" | "flight" | "flight_spike_ms" => "diagnostics",
        k if k.starts_with("bind_") => "input",
        _ => "performance",
    }
}

/// text with `key = value`; an empty value comments the key's active lines out (back to the default).
pub fn set(text: &str, key: &str, value: &str) -> String {
    let crlf = text.contains("\r\n");
    let bom = text.starts_with('\u{feff}');
    let mut lines: Vec<String> = text.trim_start_matches('\u{feff}').lines().map(String::from).collect();
    let new_line = format!("{key} = {value}");
    let active: Vec<usize> = (0..lines.len()).filter(|&i| parse(&lines[i]).is_some_and(|(k, _)| k == key)).collect();
    if value.is_empty() {
        for &i in &active {
            lines[i] = format!("# {}", lines[i].trim_start());
        }
    } else if let Some((&first, rest)) = active.split_first() {
        lines[first] = new_line;
        for &i in rest.iter().rev() {   // a later line would win over ours
            lines.remove(i);
        }
    } else if let Some(i) = (0..lines.len()).find(|&i| commented(&lines[i]).is_some_and(|k| k == key)) {
        lines[i] = new_line;
    } else {
        let want = section_for(key);
        match (0..lines.len()).find(|&i| section_of(&lines[i]).is_some_and(|s| s == want)) {
            Some(i) => lines.insert(i + 1, new_line),
            None => {
                if lines.last().is_some_and(|l| !l.trim().is_empty()) {
                    lines.push(String::new());
                }
                lines.push(format!("[{want}]"));
                lines.push(new_line);
            }
        }
    }
    let nl = if crlf { "\r\n" } else { "\n" };
    let mut out = if bom { String::from('\u{feff}') } else { String::new() };
    out += &lines.join(nl);
    out += nl;
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    const EXAMPLE: &str = "# header\n[frame rate]\n# fps_limit = 60\n\n[display]\n# fullscreen = 0\n";

    #[test]
    fn activates_the_commented_default() {
        let t = set(EXAMPLE, "fps_limit", "120");
        assert!(t.contains("\nfps_limit = 120\n"));
        assert!(!t.contains("# fps_limit"));
        assert_eq!(read(&t)["fps_limit"], "120");
    }

    #[test]
    fn rewrites_in_place_and_drops_later_duplicates() {
        let t = set("TOTK_FULLSCREEN = 1\n[display]\nfullscreen=0\n", "fullscreen", "1");
        assert_eq!(t, "fullscreen = 1\n[display]\n");
    }

    #[test]
    fn empty_value_comments_out() {
        let t = set("[input]\nmouse_sens = 3\n", "mouse_sens", "");
        assert_eq!(t, "[input]\n# mouse_sens = 3\n");
        assert!(read(&t).get("mouse_sens").is_none());
    }

    #[test]
    fn adds_to_an_existing_or_new_section() {
        let t = set(EXAMPLE, "upscaler", "bilinear");
        assert!(t.contains("[display]\nupscaler = bilinear\n"));
        let t = set("a = 1\n", "romfs_mods", "C:\\mods");
        assert_eq!(t, "a = 1\n\n[files]\nromfs_mods = C:\\mods\n");
    }

    #[test]
    fn keeps_crlf_bom_and_quotes() {
        let t = set("\u{feff}[audio]\r\nvolume = \"80\"\r\n", "lang", "pt-BR");
        assert!(t.starts_with('\u{feff}') && t.contains("\r\n[language]\r\nlang = pt-BR\r\n"));
        assert_eq!(read(&t)["volume"], "80");
    }
}
