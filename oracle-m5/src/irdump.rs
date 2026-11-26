// SPDX-License-Identifier: GPL-3.0-or-later

use std::fmt::Write as _;

use victory_shader::gfx10::{Dpp, Fields, Operand};
use victory_shader::ir::{Exit, Program, Reg, Value};

pub fn op(o: &Operand) -> String {
    match *o {
        Operand::Scalar { code, dwords } => format!("S{code}x{dwords}"),
        Operand::Vector { reg, dwords } => format!("V{reg}x{dwords}"),
        Operand::Const(c) => format!("C{c}"),
        Operand::Literal(l) => format!("L{l:x}"),
        Operand::Attr { index, chan, high } => format!("A{index}.{chan}{}", if high { "h" } else { "" }),
    }
}

fn opt(o: &Option<Operand>) -> String {
    o.as_ref().map_or_else(|| "-".to_owned(), op)
}

fn ops(list: &[Operand]) -> String {
    let parts: Vec<String> = list.iter().map(op).collect();
    format!("[{}]", parts.join(","))
}

fn b(x: bool) -> u8 {
    u8::from(x)
}

pub fn fields(f: &Fields) -> String {
    match f {
        Fields::Sop { dst, src, imm } => {
            format!("sop {} {} {}", opt(dst), ops(src), imm.map_or_else(|| "-".to_owned(), |i| i.to_string()))
        }
        Fields::Sopp { imm } => format!("sopp {imm}"),
        Fields::Smem { data, base, offset, soffset, cache } => format!(
            "smem {} {} {offset} {} {}{}{}",
            opt(data),
            op(base),
            op(soffset),
            b(cache.glc),
            b(cache.slc),
            b(cache.dlc)
        ),
        Fields::Valu(v) => {
            let mut s = format!(
                "valu {} {} {} {} {} {} {} {} {} {}",
                opt(&v.dst),
                opt(&v.carry),
                ops(&v.src),
                v.neg,
                v.abs,
                v.neg_hi,
                v.opsel,
                v.opsel_hi,
                b(v.clamp),
                v.omod
            );
            if let Some(w) = v.sdwa {
                let _ = write!(s, " sdwa {} {} {} {} {}", w.dst_sel, w.dst_unused, w.src_sel[0], w.src_sel[1], w.sext);
            }
            match v.dpp {
                Some(Dpp::Row { ctrl, row_mask, bank_mask, bound_ctrl, fi }) => {
                    let _ = write!(s, " dpp {ctrl} {row_mask} {bank_mask} {} {}", b(bound_ctrl), b(fi));
                }
                Some(Dpp::Lanes { sel, fi }) => {
                    let _ = write!(s, " dpp8 {sel} {}", b(fi));
                }
                None => {}
            }
            s
        }
        Fields::Vintrp { dst, src, attr } => format!("vintrp {} {} {}", op(dst), op(src), op(attr)),
        Fields::Ds { dst, addr, data, offset0, offset1, gds } => {
            format!("ds {} {} {} {offset0} {offset1} {}", opt(dst), opt(addr), ops(data), b(*gds))
        }
        Fields::Buffer { data, addr, rsrc, soffset, offset, offen, idxen, format, lds, tfe, cache } => format!(
            "buf {} {} {} {} {offset} {}{} {} {}{} {}{}{}",
            opt(data),
            op(addr),
            op(rsrc),
            op(soffset),
            b(*offen),
            b(*idxen),
            format.map_or_else(|| "-".to_owned(), |f| f.to_string()),
            b(*lds),
            b(*tfe),
            b(cache.glc),
            b(cache.slc),
            b(cache.dlc)
        ),
        Fields::Image { data, addr, rsrc, sampler, dmask, dim, unorm, r128, a16, d16, tfe, lwe, cache } => format!(
            "img {} {} {} {} {dmask} {dim} {}{}{}{}{}{} {}{}{}",
            op(data),
            ops(addr),
            op(rsrc),
            opt(sampler),
            b(*unorm),
            b(*r128),
            b(*a16),
            b(*d16),
            b(*tfe),
            b(*lwe),
            b(cache.glc),
            b(cache.slc),
            b(cache.dlc)
        ),
        Fields::Flat { dst, addr, data, saddr, offset, lds, cache } => format!(
            "flat {} {} {} {} {offset} {} {}{}{}",
            opt(dst),
            opt(addr),
            opt(data),
            opt(saddr),
            b(*lds),
            b(cache.glc),
            b(cache.slc),
            b(cache.dlc)
        ),
        Fields::Exp { target, src, compr, done, vm } => {
            format!("exp {target} {} {} {} {} {}{}{}", opt(&src[0]), opt(&src[1]), opt(&src[2]), opt(&src[3]), b(*compr), b(*done), b(*vm))
        }
    }
}

pub fn reg(r: Reg) -> String {
    match r {
        Reg::S(c) => format!("s{c}"),
        Reg::Scc => "scc".to_owned(),
        Reg::V(v) => format!("v{v}"),
    }
}

fn rv(list: &[(Reg, Value)]) -> String {
    let parts: Vec<String> = list.iter().map(|(r, v)| format!("{}={}", reg(*r), v.0)).collect();
    parts.join(" ")
}

pub fn program(p: &Program, out: &mut String) {
    let _ = writeln!(out, " wave64 {} vgprs {}", b(p.wave64), p.vgprs);
    for (i, block) in p.blocks.iter().enumerate() {
        let preds: Vec<String> = block.preds.iter().map(|p| p.to_string()).collect();
        let exit = match block.exit {
            Exit::Jump(t) => format!("jump {t}"),
            Exit::Branch { taken, not_taken } => format!("branch {taken} {not_taken}"),
            Exit::End => "end".to_owned(),
            Exit::Indirect => "indirect".to_owned(),
        };
        let _ = writeln!(out, " block {i} {:#x} preds {} exit {exit}", block.offset, preds.join(","));
        for phi in &block.phis {
            let args: Vec<String> = phi.args.iter().map(|a| a.0.to_string()).collect();
            let _ = writeln!(out, "  phi {}={} {}", reg(phi.reg), phi.value.0, args.join(","));
        }
        for inst in &block.insts {
            let words: Vec<String> = inst.machine.words.iter().map(|w| format!("{w:08x}")).collect();
            let _ = writeln!(
                out,
                "  {:#x} {} {} | {} | r {} | w {}",
                inst.machine.offset,
                inst.name(),
                words.join(""),
                fields(&inst.fields),
                rv(&inst.reads),
                rv(&inst.writes)
            );
        }
    }
    let _ = writeln!(out, " inputs {}", rv(&p.inputs));
}
