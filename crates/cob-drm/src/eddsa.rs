//! EdDSA over Curve25519 with **Blake2b-512** as the hash -- exactly what
//! Monocypher's `crypto_eddsa_*` does, and what every Cob license so far was
//! signed with. NOTE this is *not* standard Ed25519 (which uses SHA-512): a
//! stock Ed25519 library will not verify these signatures, and this code will
//! not verify standard ones.

use blake2::{Blake2b512, Digest};
use curve25519_dalek::constants::ED25519_BASEPOINT_POINT;
use curve25519_dalek::edwards::{CompressedEdwardsY, EdwardsPoint};
use curve25519_dalek::scalar::Scalar;

fn h512(parts: &[&[u8]]) -> [u8; 64] {
    let mut h = Blake2b512::new();
    for p in parts {
        h.update(p);
    }
    h.finalize().into()
}

/// Secret scalar `a` and the "prefix" half of the expanded seed.
fn expand(seed: &[u8; 32]) -> (Scalar, [u8; 32]) {
    let h = h512(&[seed]);
    let mut a = [0u8; 32];
    a.copy_from_slice(&h[..32]);
    a[0] &= 248;
    a[31] &= 127;
    a[31] |= 64;
    let mut prefix = [0u8; 32];
    prefix.copy_from_slice(&h[32..]);
    (Scalar::from_bytes_mod_order(a), prefix)
}

/// Public key for a 32-byte secret seed.
pub fn public_key(seed: &[u8; 32]) -> [u8; 32] {
    let (a, _) = expand(seed);
    (ED25519_BASEPOINT_POINT * a).compress().to_bytes()
}

pub fn sign(seed: &[u8; 32], msg: &[u8]) -> [u8; 64] {
    let (a, prefix) = expand(seed);
    let pk = (ED25519_BASEPOINT_POINT * a).compress().to_bytes();
    let r = Scalar::from_bytes_mod_order_wide(&h512(&[&prefix, msg]));
    let big_r = (ED25519_BASEPOINT_POINT * r).compress().to_bytes();
    let k = Scalar::from_bytes_mod_order_wide(&h512(&[&big_r, &pk, msg]));
    let s = r + k * a;
    let mut sig = [0u8; 64];
    sig[..32].copy_from_slice(&big_r);
    sig[32..].copy_from_slice(s.as_bytes());
    sig
}

pub fn verify(sig: &[u8; 64], pk: &[u8; 32], msg: &[u8]) -> bool {
    let mut r_bytes = [0u8; 32];
    r_bytes.copy_from_slice(&sig[..32]);
    let mut s_bytes = [0u8; 32];
    s_bytes.copy_from_slice(&sig[32..]);
    // the scalar must be canonical (< group order), as Monocypher requires
    let s: Option<Scalar> = Scalar::from_canonical_bytes(s_bytes).into();
    let Some(s) = s else { return false };
    let Some(a): Option<EdwardsPoint> = CompressedEdwardsY(*pk).decompress() else { return false };
    let k = Scalar::from_bytes_mod_order_wide(&h512(&[&r_bytes, pk, msg]));
    // R' = [s]B - [k]A must encode to R
    let r_check = EdwardsPoint::vartime_double_scalar_mul_basepoint(&k, &(-a), &s);
    r_check.compress().to_bytes() == r_bytes
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_and_tamper() {
        let seed = [7u8; 32];
        let pk = public_key(&seed);
        let sig = sign(&seed, b"hello");
        assert!(verify(&sig, &pk, b"hello"));
        assert!(!verify(&sig, &pk, b"hellp"));
        let mut bad = sig;
        bad[10] ^= 1;
        assert!(!verify(&bad, &pk, b"hello"));
        assert!(!verify(&sig, &public_key(&[8u8; 32]), b"hello"));
    }

    #[test]
    fn deterministic() {
        let seed = [1u8; 32];
        assert_eq!(sign(&seed, b"x"), sign(&seed, b"x"));
    }

    /// KNOWN-ANSWER TEST against the original C implementation (Monocypher's
    /// `crypto_eddsa_*`, as used by the C `cob_sign`). The seed and signature
    /// below were produced by that C tool; EdDSA is deterministic, so this Rust
    /// code must reproduce the signature byte for byte AND verify it. If this
    /// ever fails, licenses issued with the C tool no longer verify.
    #[test]
    fn matches_monocypher_known_answer() {
        fn unhex(s: &str) -> Vec<u8> {
            (0..s.len() / 2).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap()).collect()
        }
        let seed: [u8; 32] = unhex("3a318dd99ae39ff01cd3799f3ccd37a85714aef65017b7c05878b35f92c90e1b").try_into().unwrap();
        let msg = b"COBLIC1\ncustomer=Same Inputs\nissued=2026-10-31\nexpires=never\nfeatures=*\nseats=3\nkey_id=1\n";
        let c_sig_b64 = "05kUjcO9JghP3mQ+V6dEuztSFqYwMJV/vJdsMeIWUNBxmZCHUohs7TFB+AZLz/6vYhdHxq2nTi3KUPD4vG2RBg==";
        let c_sig: [u8; 64] = crate::license::b64decode(c_sig_b64).unwrap().try_into().unwrap();
        assert_eq!(sign(&seed, msg), c_sig, "Rust signature differs from the C/Monocypher one");
        assert!(verify(&c_sig, &public_key(&seed), msg), "Rust does not verify a C-made signature");
    }
}
