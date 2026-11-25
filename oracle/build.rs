
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use syn::spanned::Spanned;



struct Source {
    path: PathBuf,
    text: String,
    file: syn::File,
    line_starts: Vec<usize>,
}

impl Source {
    fn load(path: PathBuf) -> Source {
        println!("cargo:rerun-if-changed={}", path.display());
        let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        let file = syn::parse_file(&text).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        let mut line_starts = vec![0];
        for (i, b) in text.bytes().enumerate() {
            if b == b'\n' {
                line_starts.push(i + 1);
            }
        }
        Source { path, text, file, line_starts }
    }

         fn offset(&self, at: proc_macro2::LineColumn) -> usize {
        let line = self.line_starts[at.line - 1];
        let rest = &self.text[line..];
        line + rest.char_indices().nth(at.column).map_or(rest.len(), |(i, _)| i)
    }

    fn slice(&self, span: proc_macro2::Span) -> &str {
        let (from, to) = (self.offset(span.start()), self.offset(span.end()));
        &self.text[from..to]
    }

    fn sha256(&self) -> String {
        sha256_hex(self.text.as_bytes())
    }
}

fn item_name(item: &syn::Item) -> Option<String> {
    Some(match item {
        syn::Item::Const(i) => i.ident.to_string(),
        syn::Item::Static(i) => i.ident.to_string(),
        syn::Item::Fn(i) => i.sig.ident.to_string(),
        syn::Item::Struct(i) => i.ident.to_string(),
        syn::Item::Enum(i) => i.ident.to_string(),
        syn::Item::Type(i) => i.ident.to_string(),

        syn::Item::Impl(i) if i.trait_.is_none() => match &*i.self_ty {
            syn::Type::Path(p) => format!("impl {}", p.path.segments.last()?.ident),
            _ => return None,
        },

        syn::Item::ForeignMod(m) => match m.items.first()? {
            syn::ForeignItem::Fn(f) => format!("extern {}", f.sig.ident),
            _ => return None,
        },
        _ => return None,
    })
}

  fn take_items(src: &Source, names: &[&str], renames: &[(&str, &str)], manifest: &mut String) -> String {
    let mut out = String::new();
    for &name in names {
        let found: Vec<&syn::Item> = src.file.items.iter().filter(|i| item_name(i).as_deref() == Some(name)).collect();
        assert_eq!(found.len(), 1, "{}: expected exactly one item `{name}`, found {}", src.path.display(), found.len());
        let span = found[0].span();
        let mut text = src.slice(span).to_string();
        if let Some(&(_, to)) = renames.iter().find(|(from, _)| *from == name) {
            let needle = format!("fn {name}(");
            assert!(text.contains(&needle), "rename of `{name}`");
            text = text.replacen(&needle, &format!("fn {to}("), 1);
            writeln!(manifest, "  {name} (lines {}-{}, renamed {to})", span.start().line, span.end().line).unwrap();
        } else {
            writeln!(manifest, "  {name} (lines {}-{})", span.start().line, span.end().line).unwrap();
        }
        writeln!(out, "// ---- {} : {name} (line {}) ----", file_name(&src.path), span.start().line).unwrap();
        out.push_str(&text);
        out.push_str("\n\n");
    }
    out
}

  fn take_methods(src: &Source, ty: &str, names: &[&str], manifest: &mut String) -> String {
    let mut out = format!("impl {ty} {{\n");
    for &name in names {
        let mut found = Vec::new();
        for item in &src.file.items {
            let syn::Item::Impl(imp) = item else { continue };
            if imp.trait_.is_some() || item_name(item).as_deref() != Some(&format!("impl {ty}")) {
                continue;
            }
            for it in &imp.items {
                if let syn::ImplItem::Fn(f) = it
                    && f.sig.ident == name
                {
                    found.push(f);
                }
            }
        }
        assert_eq!(found.len(), 1, "{}: expected exactly one `{ty}::{name}`, found {}", src.path.display(), found.len());
        let span = found[0].span();
        writeln!(manifest, "  {ty}::{name} (lines {}-{})", span.start().line, span.end().line).unwrap();
        writeln!(out, "    // ---- {} : {ty}::{name} (line {}) ----", file_name(&src.path), span.start().line).unwrap();

        out.push_str("    ");
        out.push_str(src.slice(span));
        out.push_str("\n\n");
    }
    out.push_str("}\n");
    out
}

fn file_name(p: &Path) -> String {
    p.file_name().unwrap().to_string_lossy().into_owned()
}

fn main() {
    println!("cargo:rerun-if-env-changed=DANZI_REFERENCE");
    let reference = std::env::var("DANZI_REFERENCE").expect("Set DANZI_REFERENCE to the Rust reference checkout");
    let src = Path::new(&reference).join("crates").join("victory-gpu").join("src");
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let mut manifest = format!("reference: {reference}\n");


    let watch = Source::load(src.join("watch.rs"));
    writeln!(manifest, "watch.rs sha256 {} (whole file)", watch.sha256()).unwrap();
    let watch_text: String = watch
        .text
        .lines()
        .map(|l| if let Some(rest) = l.strip_prefix("//!") { format!("//{rest}\n") } else { format!("{l}\n") })
        .collect();
    std::fs::write(out.join("watch.rs"), watch_text).unwrap();

    let inplace = Source::load(src.join("inplace.rs"));
    writeln!(manifest, "inplace.rs sha256 {}", inplace.sha256()).unwrap();
    let inplace_items = take_items(
        &inplace,
        &[
            "K64",
            "Views",
            "VIEWS",
            "views",
            "title_memory_section",
            "title_memory_view",
            "views_gone",
            "pages_of",
            "alias_blocks_in",
            "views_of_section",

            "level",
            "most",
            "STATS",
            "Placed",
            "place_part",
            "place_for",
            "gone",
            "title_memory_freeing",
            "From",
            "open",
            "buffer_desc",
            "place",
            "Cover",
            "cover_in",
            "cover",
            "private_whole",
        ],
        &[],
        &mut manifest,
    );
    std::fs::write(out.join("inplace_reference.rs"), inplace_items).unwrap();

    let host = Source::load(src.join("host.rs"));
    writeln!(manifest, "host.rs sha256 {}", host.sha256()).unwrap();
    let host_items = take_items(
        &host,
        &[
            "copy_mapped",
            "read_sparse",
            "read_pieces",
            "read_into",
            "read",
            "extern NtReadVirtualMemory",
            "for_pieces",

            "direct_writes",
            "write_mapped",
            "COPY_BACK",
            "Declared",
            "join_ranges",
            "hand_back",
            "relay_hand_back",
        ],
        &[("copy_mapped", "copy_mapped_reference"), ("write_mapped", "write_mapped_reference")],
        &mut manifest,
    );
    std::fs::write(out.join("host_reference.rs"), host_items).unwrap();


    let mut card_items = take_items(
        &host,
        &[
            "MADE",
            "LISTS",
            "Prelude",
            "CARD_INLINE_HEADER",
            "CARD_INLINE_STRIDE",
            "CARD_INLINE_SLOTS",
            "card_inline_fetch",
            "CardWork",
            "CARD_WORK",
            "CARD_COMPARE",
            "rehash_runs",
            "CARD_PASSES",
            "wide_merge",
            "CardRange",
            "CardPlan",
            "Step",
            "QUEUE_ORDER",
            "late_stage",
            "relay",
            "transition",
            "RelayFetch",

            "RELAY_STATS",
        ],
        &[],
        &mut manifest,
    );
    card_items.push_str(&take_methods(
        &host,
        "Host",
        &[
            "make_prelude",
            "card_ready",
            "make_card_work",
            "card_before",
            "card_results_low",
            "card_done",
            "make_relay_fetch",
            "relay_card_back",
            "card_back",
            "shared_buffer",
            "uav_buffer",

            "gpu_upload_buffer",
        ],
        &mut manifest,
    ));
    std::fs::write(out.join("host_card_reference.rs"), card_items).unwrap();

    let shadow = Source::load(src.join("shadow.rs"));
    writeln!(manifest, "shadow.rs sha256 {}", shadow.sha256()).unwrap();
    let mut shadow_items = take_items(
        &shadow,
        &[
            "CopiedPages",
            "impl CopiedPages",
            "relay",
            "PREPARED",
            "preparing",
            "Flights",
            "impl Flights",
            "POOL",
            "pooled",
            "give_back",
            "NOT_WATCHED",
            "VIEW_GUARDED",
            "PIECE",
            "copy_wide",
            "wide_back",
            "SNAPS",
            "BACK_CLASS",
            "keeps",
            "KEPT_BYTES",
            "kept_most",
            "pieces_of",
            "piece_at",

            "outside",
            "merge_piece",
            "first_difference",
            "last_difference",
            "Job",
            "Workers",
            "workers",
            "wide",
            "wide_run",
            "WIDE_JOB",
            "take_snapshots",
            "copy_jobs",
            "find_all",
            "find_pieces",
            "MERGED",
            "changed_runs",

            "Card",
            "cards",
            "card_limits",
            "CARD_BYTES",
            "CARDED",
            "CARD_STATS",
            "card_stats",
            "card_hash_bytes",
            "card_vram_percent",
            "card_for",
            "WINDOW_CARD_BYTES",
            "card_dirty",
            "joined",
            "joined_piece_runs",
            "CardSpan",
            "card_span_of",
            "card_piece_of",
            "RELAY_FETCHED",
            "relay_fetched",

            "Entry",
            "BUFFERS",
            "buffers",
            "register",
            "unregister",
            "find",
            "find_any",
            "lookup",
            "rebar_copy",
            "WINDOW",
            "window",
            "grid",
            "window_of",
            "tails",
            "TAIL_STEP",
            "tail_of",
            "holder",
            "fine",
            "cover",
            "longest_tail",
            "LOOSE",
            "loose",
            "window_card_most",
            "window_card_for",
            "in_place_let_go",
        ],
        &[],
        &mut manifest,
    );
    shadow_items.push_str(&take_methods(
        &shadow,
        "Shadow",
        &[
            "unwatched",
            "is_view_guarded",
            "inside",
            "writing",
            "copy_back_done",
            "pieces",
            "piece",
            "changed",
            "writes_pending",
            "stale_under_writes",
            "update",
            "update_free",
            "copy_stale",
            "copy",

            "carded",
            "copy_back",
            "copy_back_with",
            "compared",
            "relay_compared",
            "relay_merge",
            "differing",
            "merge_under_writes",

            "card_changed",
            "card_sides",
            "card_make_before",
            "card_span",
            "card_run",
            "card_piece",
            "card_merge",
            "card_merge_relay",
        ],
        &mut manifest,
    ));
    std::fs::write(out.join("shadow_reference.rs"), shadow_items).unwrap();


    let rebar = Source::load(src.join("rebar.rs"));
    writeln!(manifest, "rebar.rs sha256 {} (whole file)", rebar.sha256()).unwrap();
    let rebar_text: String = rebar
        .text
        .lines()
        .map(|l| if let Some(rest) = l.strip_prefix("//!") { format!("//{rest}\n") } else { format!("{l}\n") })
        .collect();
    std::fs::write(out.join("rebar.rs"), rebar_text).unwrap();
    let memcopy = Source::load(src.join("memcopy.rs"));
    writeln!(manifest, "memcopy.rs sha256 {}", memcopy.sha256()).unwrap();
    let memcopy_items = take_items(
        &memcopy,
        &["RangeSet", "impl RangeSet", "LANES", "K", "lane_hash", "check_turn", "hash_every_of"],
        &[],
        &mut manifest,
    );
    std::fs::write(out.join("memcopy_reference.rs"), memcopy_items).unwrap();
    let readback = Source::load(src.join("readback.rs"));
    writeln!(manifest, "readback.rs sha256 {}", readback.sha256()).unwrap();
    let readback_items = take_items(&readback, &["TAGGED", "tag", "tagged_in"], &[], &mut manifest);
    std::fs::write(out.join("readback_reference.rs"), readback_items).unwrap();
    std::fs::write(out.join("manifest.txt"), manifest).unwrap();
}


fn sha256_hex(data: &[u8]) -> String {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    ];
    let mut h: [u32; 8] =
        [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
    let mut msg = data.to_vec();
    let bits = (data.len() as u64).wrapping_mul(8);
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bits.to_be_bytes());
    for chunk in msg.chunks(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(chunk[i * 4..i * 4 + 4].try_into().unwrap());
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let mut v = h;
        for i in 0..64 {
            let s1 = v[4].rotate_right(6) ^ v[4].rotate_right(11) ^ v[4].rotate_right(25);
            let ch = (v[4] & v[5]) ^ (!v[4] & v[6]);
            let t1 = v[7].wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = v[0].rotate_right(2) ^ v[0].rotate_right(13) ^ v[0].rotate_right(22);
            let maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            let t2 = s0.wrapping_add(maj);
            v = [t1.wrapping_add(t2), v[0], v[1], v[2], v[3].wrapping_add(t1), v[4], v[5], v[6]];
        }
        for i in 0..8 {
            h[i] = h[i].wrapping_add(v[i]);
        }
    }
    h.iter().map(|x| format!("{x:08x}")).collect()
}
