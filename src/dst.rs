// SPDX-License-Identifier: LGPL-2.1-or-later
//! Direct Stream Transfer (DST) decoder: lossless DSD decompression used by
//! most multichannel (and some stereo) SACD areas.
//!
//! Rust port of FFmpeg's libavcodec/dstdec.c
//! (Copyright (c) 2014 Peter Ross <pross@xvid.org>), which is licensed under the
//! GNU Lesser General Public License v2.1 or later; this module keeps that license.
//! Reference: ISO/IEC 14496-3 Part 3 Subpart 10.
//!
//! Output is raw DSD in SACD's native layout: MSB-first, byte-interleaved channels.

use anyhow::{Result, bail, ensure};

const MAX_CHANNELS: usize = 6;
const MAX_ELEMENTS: usize = 2 * MAX_CHANNELS;
/// DSD64: 588 * 64 one-bit samples per channel per frame
pub const SAMPLES_PER_FRAME: usize = 588 * 64;

const FSETS_CODE_PRED_COEFF: [[i32; 3]; 3] = [[-8, 0, 0], [-16, 8, 0], [-9, -5, 6]];
const PROBS_CODE_PRED_COEFF: [[i32; 3]; 3] = [[-8, 0, 0], [-16, 8, 0], [-24, 24, -8]];

struct Bits<'a> {
    data: &'a [u8],
    pos: usize, // bit position
}

impl<'a> Bits<'a> {
    fn new(data: &'a [u8]) -> Self {
        Bits { data, pos: 0 }
    }
    fn left(&self) -> isize {
        (self.data.len() * 8) as isize - self.pos as isize
    }
    fn bit(&mut self) -> u32 {
        // reading past the end yields zeros, like FFmpeg's padded bit reader
        let b = self.data.get(self.pos >> 3).map_or(0, |&x| (x >> (7 - (self.pos & 7))) & 1);
        self.pos += 1;
        b as u32
    }
    fn bits(&mut self, n: u32) -> u32 {
        let mut v = 0u32;
        for _ in 0..n {
            v = (v << 1) | self.bit();
        }
        v
    }
    fn sbits(&mut self, n: u32) -> i32 {
        let v = self.bits(n);
        ((v << (32 - n)) as i32) >> (32 - n)
    }
    /// JPEG-LS style Rice code: unary prefix of zeros terminated by a one, then k bits.
    fn ur_golomb(&mut self, k: u32) -> Result<i32> {
        let limit = self.left();
        let mut q: i32 = 0;
        while self.bit() == 0 {
            q += 1;
            if q as isize >= limit {
                bail!("golomb code overruns frame");
            }
        }
        let rem = if k > 0 { self.bits(k) as i32 } else { 0 };
        Ok((q << k) + rem)
    }
    fn sr_golomb(&mut self, k: u32) -> Result<i32> {
        let v = self.ur_golomb(k)?;
        Ok(if v != 0 && self.bit() == 1 { -v } else { v })
    }
}

fn log2(x: u32) -> u32 {
    if x == 0 { 0 } else { 31 - x.leading_zeros() }
}

struct Table {
    elements: usize,
    length: [usize; MAX_ELEMENTS],
    coeff: Box<[[i32; 128]; MAX_ELEMENTS]>,
}

impl Table {
    fn new() -> Self {
        Table { elements: 0, length: [0; MAX_ELEMENTS], coeff: Box::new([[0; 128]; MAX_ELEMENTS]) }
    }
}

fn read_map(gb: &mut Bits, t: &mut Table, map: &mut [usize; MAX_CHANNELS], channels: usize) -> Result<()> {
    t.elements = 1;
    map[0] = 0;
    if gb.bit() == 0 {
        for m in map.iter_mut().take(channels).skip(1) {
            let bits = log2(t.elements as u32) + 1;
            *m = gb.bits(bits) as usize;
            if *m == t.elements {
                t.elements += 1;
                ensure!(t.elements < MAX_ELEMENTS, "too many elements");
            } else if *m > t.elements {
                bail!("bad channel map");
            }
        }
    } else {
        *map = [0; MAX_CHANNELS];
    }
    Ok(())
}

#[allow(clippy::needless_range_loop)] // mirrors FFmpeg's loop structure
fn read_table(gb: &mut Bits, t: &mut Table, pred: &[[i32; 3]; 3], length_bits: u32, coeff_bits: u32, signed: bool, offset: i32) -> Result<()> {
    for i in 0..t.elements {
        t.length[i] = gb.bits(length_bits) as usize + 1;
        let uncoded = |gb: &mut Bits, n: usize, dst: &mut [i32; 128]| {
            for d in dst.iter_mut().take(n) {
                *d = if signed { gb.sbits(coeff_bits) } else { gb.bits(coeff_bits) as i32 } + offset;
            }
        };
        if gb.bit() == 0 {
            let n = t.length[i];
            uncoded(gb, n, &mut t.coeff[i]);
        } else {
            let method = gb.bits(2) as usize;
            ensure!(method != 3, "bad coding method");
            uncoded(gb, method + 1, &mut t.coeff[i]);
            let lsb_size = gb.bits(3);
            for j in method + 1..t.length[i] {
                let mut x: i32 = 0;
                for k in 0..=method {
                    x = x.wrapping_add(pred[method][k].wrapping_mul(t.coeff[i][j - k - 1]));
                }
                let mut c = gb.sr_golomb(lsb_size)?;
                if x >= 0 {
                    c -= (x + 4) / 8;
                } else {
                    c += (-x + 3) / 8;
                }
                if !signed && (c < offset || c >= offset + (1 << coeff_bits)) {
                    bail!("probability out of range");
                }
                t.coeff[i][j] = c;
            }
        }
    }
    Ok(())
}

struct Ac {
    a: u32,
    c: u32,
}

impl Ac {
    fn init(gb: &mut Bits) -> Ac {
        Ac { a: 4095, c: gb.bits(12) }
    }
    #[inline(always)]
    fn get(&mut self, gb: &mut Bits, p: u32) -> u32 {
        let k = (self.a >> 8) | ((self.a >> 7) & 1);
        let q = k * p;
        let a_q = self.a.wrapping_sub(q);
        let e = (self.c < a_q) as u32;
        if e == 1 {
            self.a = a_q;
        } else {
            self.a = q;
            self.c = self.c.wrapping_sub(a_q);
        }
        if self.a < 2048 {
            let n = 11 - log2(self.a);
            self.a <<= n;
            self.c = (self.c << n) | gb.bits(n);
        }
        e
    }
}

fn prob_dst_x_bit(c: i32) -> u32 {
    (((c & 127) as u8).reverse_bits() >> 1) as u32 + 1
}

/// Reusable decoder state (tables are rebuilt per frame; buffers are reused).
pub struct DstDecoder {
    channels: usize,
    fsets: Table,
    probs: Table,
    filter: Vec<[[i16; 256]; 16]>,
}

impl DstDecoder {
    pub fn new(channels: usize) -> Result<Self> {
        ensure!((1..=MAX_CHANNELS).contains(&channels), "DST: unsupported channel count {channels}");
        Ok(DstDecoder { channels, fsets: Table::new(), probs: Table::new(), filter: vec![[[0; 256]; 16]; MAX_ELEMENTS] })
    }

    /// Decode one DST frame into `out` (len = SAMPLES_PER_FRAME/8 * channels).
    pub fn decode(&mut self, frame: &[u8], out: &mut [u8]) -> Result<()> {
        let channels = self.channels;
        let total = SAMPLES_PER_FRAME / 8 * channels;
        ensure!(out.len() == total, "output buffer size");
        ensure!(frame.len() > 1, "empty DST frame");
        let mut gb = Bits::new(frame);

        if gb.bit() == 0 {
            // uncompressed frame: raw DSD follows the first byte
            gb.bit();
            ensure!(gb.bits(6) == 0, "bad uncompressed DST frame header");
            let n = (frame.len() - 1).min(total);
            out[..n].copy_from_slice(&frame[1..1 + n]);
            out[n..].fill(0x69);
            return Ok(());
        }
        ensure!(gb.bit() == 1, "DST: 'not same segmentation' unsupported");
        ensure!(gb.bit() == 1, "DST: 'not same segmentation for all channels' unsupported");
        ensure!(gb.bit() == 1, "DST: 'not end of channel segmentation' unsupported");

        let same_map = gb.bit() == 1;
        let mut map_f = [0usize; MAX_CHANNELS];
        let mut map_p = [0usize; MAX_CHANNELS];
        read_map(&mut gb, &mut self.fsets, &mut map_f, channels)?;
        if same_map {
            self.probs.elements = self.fsets.elements;
            map_p = map_f;
        } else {
            read_map(&mut gb, &mut self.probs, &mut map_p, channels)?;
        }
        let mut half_prob = [false; MAX_CHANNELS];
        for h in half_prob.iter_mut().take(channels) {
            *h = gb.bit() == 1;
        }
        read_table(&mut gb, &mut self.fsets, &FSETS_CODE_PRED_COEFF, 7, 9, true, 0)?;
        read_table(&mut gb, &mut self.probs, &PROBS_CODE_PRED_COEFF, 6, 7, false, 1)?;
        ensure!(gb.bit() == 0, "DST: bad arithmetic-coding marker");
        let mut ac = Ac::init(&mut gb);

        // build_filter
        for i in 0..self.fsets.elements {
            let length = self.fsets.length[i] as i32;
            for j in 0..16 {
                let total_taps = (length - j as i32 * 8).clamp(0, 8) as usize;
                for k in 0..256usize {
                    let mut v: i64 = 0;
                    for l in 0..total_taps {
                        let bit = ((k >> l) & 1) as i64 * 2 - 1;
                        v += bit * self.fsets.coeff[i][j * 8 + l] as i64;
                    }
                    ensure!(v as i16 as i64 == v, "DST: filter coefficient overflow");
                    self.filter[i][j][k] = v as i16;
                }
            }
        }

        let mut status = [[0xAAAA_AAAA_AAAA_AAAAu64; 2]; MAX_CHANNELS];
        out.fill(0);
        let _ = ac.get(&mut gb, prob_dst_x_bit(self.fsets.coeff[0][0]));

        for i in 0..SAMPLES_PER_FRAME {
            for ch in 0..channels {
                let felem = map_f[ch];
                let filt = &self.filter[felem];
                let [lo, hi] = status[ch];
                let mut sum: i32 = 0;
                for x in 0..8 {
                    sum += filt[x][((lo >> (8 * x)) & 0xFF) as usize] as i32;
                    sum += filt[x + 8][((hi >> (8 * x)) & 0xFF) as usize] as i32;
                }
                let predict = sum as i16;
                let prob = if !half_prob[ch] || i >= self.fsets.length[felem] {
                    let pelem = map_p[ch];
                    let index = ((predict as i32).unsigned_abs() >> 3) as usize;
                    self.probs.coeff[pelem][index.min(self.probs.length[pelem] - 1)] as u32
                } else {
                    128
                };
                let residual = ac.get(&mut gb, prob);
                let v = (((predict as i32) >> 15) as u32 ^ residual) & 1;
                out[(i >> 3) * channels + ch] |= (v as u8) << (7 - (i & 7));
                status[ch] = [(lo << 1) | v as u64, (hi << 1) | (lo >> 63)];
            }
        }
        Ok(())
    }
}
