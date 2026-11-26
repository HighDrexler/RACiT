//! Two fixes to lifted AltiVec code (idempotent). It replaced scripts/fix_vmx_alias.py,
//! whose account this keeps.
//!
//! (1) Vector instructions whose destination is also a source read that source from a
//! snapshot. The lifter lowered most AltiVec instructions as a loop writing vD lane by
//! lane through a pointer into ctx->vr; when vD is also vA, vB or vC, source lanes were
//! overwritten before they were read. The case that showed it is vcmpequh. in A Crack in
//! Time's texture streamer (func_005E3584): its search never found a key, high-resolution
//! textures were never switched in, and VRAM compaction drew the striped ones.
//!
//! ```text
//!    { T* d=(T*)&ctx->vr[N]; T* a=(T*)&ctx->vr[N]; ... }
//! -> { alignas(16) uint8_t _vsnap[16]; memcpy(_vsnap, &ctx->vr[N], 16); T* d=(T*)&ctx->vr[N]; T* a=(T*)&_vsnap; ... }
//! ```
//!
//! (2) vpkuwus, vpkuhus, vadduws and vmulouh/vmuleuh compose their big-endian lanes from
//! bytes (they did lane arithmetic through host uint16_t*/uint32_t* reads: byte-reversed
//! values). ppu_lifter.py does both itself now; on its output this is a no-op.

use crate::{Result, base, lifted_sources};
use regex::bytes::{Captures, Regex};
use std::path::Path;
use std::sync::LazyLock;

static DEST: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"\bd\s*=\s*\(\s*(?:const\s+)?[a-z0-9_]+\s*\*\s*\)\s*&ctx->vr\[(\d+)\]").unwrap()
});

fn src_re(n: &str) -> Regex {
    Regex::new(&format!(
        r"\b([abc])\s*=\s*\(\s*(?:const\s+)?([a-z0-9_]+)\s*\*\s*\)\s*&ctx->vr\[{n}\]"
    ))
    .unwrap()
}

fn fix_line(line: &[u8]) -> (Vec<u8>, usize) {
    if crate::contains(line, b"_vsnap") || !crate::contains(line, b"&ctx->vr[") {
        return (line.to_vec(), 0);
    }
    let Some(m) = DEST.captures(line) else {
        return (line.to_vec(), 0);
    };
    let n = String::from_utf8_lossy(&m[1]).into_owned();
    let src = src_re(&n);
    if !src.is_match(line) {
        return (line.to_vec(), 0);
    }
    // The statement is the innermost { ... } that holds the destination pointer.
    let Some(start) = line[..m.get(0).unwrap().start()]
        .iter()
        .rposition(|&b| b == b'{')
    else {
        return (line.to_vec(), 0);
    };
    let body = &line[start + 1..];
    let new_body = src.replace_all(body, |c: &Captures| {
        [&c[1], b"=(", &c[2], b"*)&_vsnap"].concat()
    });
    let mut out = line[..start + 1].to_vec();
    out.extend_from_slice(
        format!(" alignas(16) uint8_t _vsnap[16]; memcpy(_vsnap, &ctx->vr[{n}], 16);").as_bytes(),
    );
    out.extend_from_slice(&new_body);
    (out, 1)
}

const SRC_RE: &str = r"(&ctx->vr\[\d+\]|&_vsnap)";

type Rule = (Regex, fn(&Captures) -> Option<String>);

fn g(c: &Captures, i: usize) -> String {
    String::from_utf8_lossy(&c[i]).into_owned()
}

static ENDIAN_RULES: LazyLock<Vec<Rule>> = LazyLock::new(|| {
    vec![
        // vpkuwus
        (
            Regex::new(&format!(
                r"uint16_t\* d=\(uint16_t\*\)&ctx->vr\[(\d+)\]; uint32_t\* a=\(uint32_t\*\){SRC_RE}; uint32_t\* b=\(uint32_t\*\){SRC_RE}; for\(int i=0;i<4;i\+\+\)\{{uint32_t v=a\[i\]; d\[i\]=\(uint16_t\)\(v>65535u\?65535u:v\);\}} for\(int i=0;i<4;i\+\+\)\{{uint32_t v=b\[i\]; d\[4\+i\]=\(uint16_t\)\(v>65535u\?65535u:v\);\}}"
            ))
            .unwrap(),
            |m| {
                Some(format!(
                    "const uint8_t* a=(const uint8_t*){}; const uint8_t* b=(const uint8_t*){}; uint8_t o[16]; \
                     for(int i=0;i<4;i++){{ uint32_t v=((uint32_t)a[4*i]<<24)|((uint32_t)a[4*i+1]<<16)|((uint32_t)a[4*i+2]<<8)|a[4*i+3]; \
                     uint32_t r=v>65535u?65535u:v; o[2*i]=(uint8_t)(r>>8); o[2*i+1]=(uint8_t)r; }} \
                     for(int i=0;i<4;i++){{ uint32_t v=((uint32_t)b[4*i]<<24)|((uint32_t)b[4*i+1]<<16)|((uint32_t)b[4*i+2]<<8)|b[4*i+3]; \
                     uint32_t r=v>65535u?65535u:v; o[8+2*i]=(uint8_t)(r>>8); o[8+2*i+1]=(uint8_t)r; }} \
                     memcpy(&ctx->vr[{}], o, 16);",
                    g(m, 2),
                    g(m, 3),
                    g(m, 1)
                ))
            },
        ),
        // vpkuhus
        (
            Regex::new(&format!(
                r"uint8_t\* d=\(uint8_t\*\)&ctx->vr\[(\d+)\]; uint16_t\* a=\(uint16_t\*\){SRC_RE}; uint16_t\* b=\(uint16_t\*\){SRC_RE}; for\(int i=0;i<8;i\+\+\)\{{uint32_t v=a\[i\]; d\[i\]=\(uint8_t\)\(v>255u\?255u:v\);\}} for\(int i=0;i<8;i\+\+\)\{{uint32_t v=b\[i\]; d\[8\+i\]=\(uint8_t\)\(v>255u\?255u:v\);\}}"
            ))
            .unwrap(),
            |m| {
                Some(format!(
                    "const uint8_t* a=(const uint8_t*){}; const uint8_t* b=(const uint8_t*){}; uint8_t o[16]; \
                     for(int i=0;i<8;i++){{ uint32_t v=(uint32_t)((a[2*i]<<8)|a[2*i+1]); o[i]=(uint8_t)(v>255u?255u:v); }} \
                     for(int i=0;i<8;i++){{ uint32_t v=(uint32_t)((b[2*i]<<8)|b[2*i+1]); o[8+i]=(uint8_t)(v>255u?255u:v); }} \
                     memcpy(&ctx->vr[{}], o, 16);",
                    g(m, 2),
                    g(m, 3),
                    g(m, 1)
                ))
            },
        ),
        // vadduws
        (
            Regex::new(&format!(
                r"uint32_t\* d=\(uint32_t\*\)&ctx->vr\[(\d+)\]; uint32_t\* a=\(uint32_t\*\){SRC_RE}; uint32_t\* b=\(uint32_t\*\){SRC_RE}; for\(int i=0;i<4;i\+\+\)\{{uint64_t r=\(uint64_t\)a\[i\]\+\(uint64_t\)b\[i\]; d\[i\]=\(uint32_t\)\(r>0xFFFFFFFFu\?0xFFFFFFFFu:r\);\}}"
            ))
            .unwrap(),
            |m| {
                Some(format!(
                    "const uint8_t* a=(const uint8_t*){}; const uint8_t* b=(const uint8_t*){}; uint8_t o[16]; \
                     for(int i=0;i<4;i++){{ uint64_t x=0,y=0; for(int k=0;k<4;k++){{ x=(x<<8)|a[4*i+k]; y=(y<<8)|b[4*i+k]; }} \
                     uint64_t r=x+y; uint32_t v=(uint32_t)(r>0xFFFFFFFFull?0xFFFFFFFFull:r); \
                     for(int k=0;k<4;k++) o[4*i+k]=(uint8_t)(v>>(8*(3-k))); }} \
                     memcpy(&ctx->vr[{}], o, 16);",
                    g(m, 2),
                    g(m, 3),
                    g(m, 1)
                ))
            },
        ),
        // vmulouh/vmuleuh. The Python pattern's \4 back-reference (b's first lane is a's) is
        // checked on the match: a mismatch is left as it is.
        (
            Regex::new(&format!(
                r"uint32_t\* d=\(uint32_t\*\)&ctx->vr\[(\d+)\]; uint16_t\* a=\(uint16_t\*\){SRC_RE}; uint16_t\* b=\(uint16_t\*\){SRC_RE}; d\[0\]=\(uint32_t\)a\[([01])\]\*\(uint32_t\)b\[([01])\]; d\[1\]=\(uint32_t\)a\[[23]\]\*\(uint32_t\)b\[[23]\]; d\[2\]=\(uint32_t\)a\[[45]\]\*\(uint32_t\)b\[[45]\]; d\[3\]=\(uint32_t\)a\[[67]\]\*\(uint32_t\)b\[[67]\];"
            ))
            .unwrap(),
            |m| {
                if m[4] != m[5] {
                    return None;
                }
                Some(format!(
                    "const uint8_t* a=(const uint8_t*){}; const uint8_t* b=(const uint8_t*){}; uint8_t o[16]; \
                     for(int j=0;j<4;j++){{ int l=2*j+{}; \
                     uint32_t x=(uint32_t)((a[l*2]<<8)|a[l*2+1]); uint32_t y=(uint32_t)((b[l*2]<<8)|b[l*2+1]); \
                     uint32_t r=x*y; for(int k=0;k<4;k++) o[j*4+k]=(uint8_t)(r>>(8*(3-k))); }} \
                     memcpy(&ctx->vr[{}], o, 16);",
                    g(m, 2),
                    g(m, 3),
                    g(m, 4),
                    g(m, 1)
                ))
            },
        ),
    ]
});

fn fix_endian(line: Vec<u8>) -> (Vec<u8>, usize) {
    let mut line = line;
    let mut n = 0;
    for (rx, rep) in ENDIAN_RULES.iter() {
        if !rx.is_match(&line) {
            continue;
        }
        let mut out = Vec::with_capacity(line.len());
        let mut last = 0;
        for c in rx.captures_iter(&line) {
            let whole = c.get(0).unwrap();
            if let Some(r) = rep(&c) {
                out.extend_from_slice(&line[last..whole.start()]);
                out.extend_from_slice(r.as_bytes());
                last = whole.end();
                n += 1;
            }
        }
        out.extend_from_slice(&line[last..]);
        line = out;
    }
    (line, n)
}

/// Rewrites the lift in place (unless `check`), printing what it changed as the Python did.
pub fn run(recomp: &Path, check: bool) -> Result<()> {
    let mut total = 0;
    for path in lifted_sources(recomp) {
        let text = crate::read(&path)?;
        // The cheap test first: almost every chunk has nothing to change.
        let pre = crate::contains(&text, b"&ctx->vr[");
        if !pre {
            continue;
        }
        let nl: &[u8] = if crate::contains(&text, b"\r\n") {
            b"\r\n"
        } else {
            b"\n"
        };
        let mut lines: Vec<Vec<u8>> = split(&text, nl);
        let mut count = 0;
        for line in lines.iter_mut() {
            if !crate::contains(line, b"&ctx->vr[") {
                continue; // neither fix touches a line without a vector register
            }
            let (new, c) = fix_line(line);
            let (new, e) = fix_endian(new);
            if c + e > 0 {
                *line = new;
                count += c + e;
            }
        }
        if count > 0 {
            total += count;
            println!("{}: {count} vector statement(s)", base(&path));
            if !check {
                crate::write(&path, &lines.join(nl))?;
            }
        }
    }
    println!(
        "{total} statement(s) {}rewritten",
        if check { "would be " } else { "" }
    );
    Ok(())
}

fn split(text: &[u8], sep: &[u8]) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    let mut last = 0;
    for i in memchr::memmem::find_iter(text, sep) {
        out.push(text[last..i].to_vec());
        last = i + sep.len();
    }
    out.push(text[last..].to_vec());
    out
}
