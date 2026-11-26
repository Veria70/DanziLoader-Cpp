// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeSet;
use std::fmt::Write as _;

use victory_shader::structure::{Atom, Bool, Bools, LaneExit, Structured};

fn id(b: Bool) -> u32 {

    let s = format!("{b:?}");
    s.trim_start_matches("Bool(").trim_end_matches(')').parse().unwrap()
}

fn atom(a: Atom) -> String {
    match a {
        Atom::Lane(v) => format!("lane{}", v.0),
        Atom::Uniform(v) => format!("uni{}", v.0),
        Atom::Phi(v) => format!("phi{}", v.0),
        Atom::Quad(v) => format!("quad{}", v.0),
        Atom::Opaque(v) => format!("opq{}", v.0),
        Atom::Constant(c) => format!("const{c:x}"),
    }
}

fn collect(bools: &Bools, b: Bool, seen: &mut BTreeSet<u32>) {
    let mut stack = vec![b];
    while let Some(b) = stack.pop() {
        if !seen.insert(id(b)) {
            continue;
        }
        if let Some((_, lo, hi)) = bools.node(b) {
            stack.push(lo);
            stack.push(hi);
        }
    }
}

pub fn structured(s: &Structured, out: &mut String) {
    let mut named = BTreeSet::new();
    for (i, e) in s.exits.iter().enumerate() {
        match *e {
            LaneExit::Jump(t) => {
                let _ = writeln!(out, " exit {i} jump {t}");
            }
            LaneExit::Branch { when, taken, not_taken } => {
                let _ = writeln!(out, " exit {i} branch {} {taken} {not_taken}", id(when));
                collect(&s.bools, when, &mut named);
            }
            LaneExit::End => {
                let _ = writeln!(out, " exit {i} end");
            }
            LaneExit::Indirect => {
                let _ = writeln!(out, " exit {i} indirect");
            }
        }
    }
    for (b, guards) in s.guards.iter().enumerate() {
        for (i, g) in guards.iter().enumerate() {
            if let Some(g) = g {
                let _ = writeln!(out, " guard {b} {i} {}", id(*g));
                collect(&s.bools, *g, &mut named);
            }
        }
    }
    for (b, r) in s.reach.iter().enumerate() {
        let _ = writeln!(out, " reach {b} {}", id(*r));
        collect(&s.bools, *r, &mut named);
    }
    let mut masks: Vec<_> = s.masks.iter().map(|(v, m)| (v.0, *m)).collect();
    masks.sort();
    for (v, m) in masks {
        let _ = writeln!(out, " mask {v} {}", id(m));
        collect(&s.bools, m, &mut named);
    }
    let mut highs: Vec<_> = s.highs.iter().map(|(h, l)| (h.0, l.0)).collect();
    highs.sort();
    for (h, l) in highs {
        let _ = writeln!(out, " high {h} {l}");
    }
    let mut active: Vec<_> = s.active.iter().map(|v| v.0).collect();
    active.sort();
    let _ = writeln!(out, " active {active:?}");
    let mut phis: Vec<_> = s.phi_masks.iter().map(|(v, m)| (v.0, m.clone())).collect();
    phis.sort_by_key(|p| p.0);
    for (v, m) in phis {
        let ids: Vec<String> = m.iter().map(|b| id(*b).to_string()).collect();
        let _ = writeln!(out, " phimask {v} {}", ids.join(","));
        for b in m {
            collect(&s.bools, b, &mut named);
        }
    }
    let mut wide: Vec<_> = s.wide.iter().copied().collect();
    wide.sort();
    let _ = writeln!(out, " wide {wide:?}");
    for (b, w) in s.wave_any.iter().enumerate() {
        if let Some(w) = w {
            let _ = writeln!(out, " wave_any {b} {}", u8::from(*w));
        }
    }
    let mut scc: Vec<_> = s.scc_some.iter().map(|(v, m)| (v.0, *m)).collect();
    scc.sort();
    for (v, m) in scc {
        let _ = writeln!(out, " scc_some {v} {}", id(m));
        collect(&s.bools, m, &mut named);
    }
    for n in named {

        let b = all_bools(&s.bools, n);
        match s.bools.node(b) {
            Some((a, lo, hi)) => {
                let _ = writeln!(out, " node {n} {} {} {}", atom(a), id(lo), id(hi));
            }
            None => {
                let _ = writeln!(out, " node {n} const");
            }
        }
    }
}

    fn all_bools(_bools: &Bools, n: u32) -> Bool {

    unsafe { std::mem::transmute::<u32, Bool>(n) }
}
