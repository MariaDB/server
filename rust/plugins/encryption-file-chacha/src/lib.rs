//! An encryption plugin using file key managment with the ChaCha20 streaming encryption algorithm.
//! When padding is allowed, ChaCha20-Poly1305 is used.

use chacha20::cipher::{KeyIvInit, StreamCipher};
use chacha20::ChaCha20;
use chacha20poly1305::aead::AeadMutInPlace;
use chacha20poly1305::{ChaCha20Poly1305, KeyInit};
use encryption_common::trunc_or_extend;
use mariadb::log::error;
use mariadb::plugin::encryption::{Decryption, Encryption, EncryptionError};
use mariadb::plugin::{
    register_plugin, License, Maturity, PluginType, SysVarConstString, SysVarOpt,
};
use mariadb::warn_once;

/// Auth tag length for chachapoly1305
const CHACHA_TAG_LEN: usize = 16;
const CHACHA_KEY_LEN: usize = 32;
const CHACHA_NONCE_LEN: usize = 12;

register_plugin! {
    FileKeyMgmt,
    ptype: PluginType::MariaEncryption,
    name: "file_key_management_chacha",
    author: "Trevor Gross",
    description: "File key managment with chacha encryption",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    version: "0.1",
    init: FileKeyMgmt,
    encryption: ChaChaCtx,
    variables: [
        SysVar {
            ident: FILE_NAME,
            vtype: SysVarConstString,
            name: "filename",
            description: "Path and name of the key file.",
            options: [SysVarOpt::ReadOnly, SysVarOpt::RequiredCliArg],
        }
    ]
}

encryption_common::create_fkmgt!(FileKeyMgmt, FILE_NAME);

/// Our encryption plugin will use the AEAD if a tag can be appended. Otherwise, the fallback is
/// the in-place streaming cipher.
enum ChaChaCtx {
    Stream(ChaCha20),
    Aead {
        cipher: ChaCha20Poly1305,
        nonce: [u8; CHACHA_NONCE_LEN],
        tag: [u8; CHACHA_TAG_LEN],
        already_called: bool,
    },
}

impl Encryption for ChaChaCtx {
    fn init(
        _key_id: u32,
        _key_version: u32,
        key: &[u8],
        iv: &[u8],
        same_size: bool,
    ) -> Result<Self, EncryptionError> {
        init_cipher(key, iv, same_size)
    }

    fn update(&mut self, src: &[u8], mut dst: &mut [u8]) -> Result<usize, EncryptionError> {
        match self {
            ChaChaCtx::Stream(ref mut cipher) => stream_apply_ks(cipher, src, dst),
            ChaChaCtx::Aead {
                cipher,
                nonce,
                tag,
                already_called,
            } => {
                if *already_called {
                    error!("chacha encryption update called more than once");
                    return Err(EncryptionError::Other);
                }
                if dst.len() > src.len() {
                    dst = &mut dst[..src.len()];
                }
                let new_tag = cipher
                    .encrypt_in_place_detached(&(*nonce).into(), b"", dst)
                    .map_err(|e| {
                        error!("chacha encryption error: {e}");
                        EncryptionError::Other
                    })?;
                *already_called = true;
                *tag = new_tag.into();
                Ok(src.len())
            }
        }
    }

    fn finish(&mut self, dst: &mut [u8]) -> Result<usize, EncryptionError> {
        match self {
            // Nop for stream
            ChaChaCtx::Stream(_) => Ok(0),
            ChaChaCtx::Aead { tag, .. } => {
                if dst.len() < tag.len() {
                    error!(
                        "insufficient dst len {} for tag of length {}",
                        dst.len(),
                        tag.len()
                    );
                    return Err(EncryptionError::Other);
                }
                dst[..tag.len()].copy_from_slice(tag);
                Ok(tag.len())
            }
        }
    }

    fn encrypted_length(_key_id: u32, _key_version: u32, src_len: usize) -> usize {
        src_len + CHACHA_TAG_LEN
    }
}

impl Decryption for ChaChaCtx {
    fn init(
        _key_id: u32,
        _key_version: u32,
        key: &[u8],
        iv: &[u8],
        same_size: bool,
    ) -> Result<Self, EncryptionError> {
        init_cipher(key, iv, same_size)
    }

    fn update(&mut self, src: &[u8], dst: &mut [u8]) -> Result<usize, EncryptionError> {
        match self {
            ChaChaCtx::Stream(cipher) => stream_apply_ks(cipher, src, dst),
            ChaChaCtx::Aead {
                cipher,
                nonce,
                already_called,
                ..
            } => {
                if *already_called {
                    error!("chacha decryption update called more than once");
                    return Err(EncryptionError::Other);
                }
                if src.len() < CHACHA_TAG_LEN {
                    error!(
                        "AES decryption requires {CHACHA_TAG_LEN} bytes but got {}",
                        dst.len()
                    );
                }

                // Tag must be at the end
                let src_data = &src[..src.len() - CHACHA_TAG_LEN];
                let src_tag = &src[src.len() - CHACHA_TAG_LEN..];

                // Dst winds up with the entire source minus the tag
                let use_dst = &mut dst[..src_data.len()];

                use_dst.copy_from_slice(&src_data);
                cipher
                    .decrypt_in_place_detached(&(*nonce).into(), b"", use_dst, src_tag.into())
                    .map_err(|e| {
                        error!("chacha decryption update error {e}");
                        EncryptionError::Other
                    })?;
                *already_called = true;
                Ok(src_data.len())
            }
        }
    }
}

fn init_cipher(in_key: &[u8], in_iv: &[u8], same_size: bool) -> Result<ChaChaCtx, EncryptionError> {
    let (key, key_ok, key_action) = trunc_or_extend::<CHACHA_KEY_LEN>(in_key);
    let (nonce, nonce_ok, nonce_action) = trunc_or_extend::<CHACHA_NONCE_LEN>(in_iv);

    if !key_ok {
        warn_once!(
            "ChaCha20 expects {CHACHA_KEY_LEN}-byte key but got {}. \
            {key_action} to meet requirements. {in_key:x?}",
            in_key.len()
        );
    }
    if !nonce_ok {
        warn_once!(
            "ChaCha20 expects {CHACHA_NONCE_LEN}-byte nonce but got {}. \
            {nonce_action} to meet requirements.",
            in_iv.len()
        );
    }

    if same_size {
        let cipher = ChaCha20::new(&key.into(), &nonce.into());
        Ok(ChaChaCtx::Stream(cipher))
    } else {
        let cipher = ChaCha20Poly1305::new(&key.into());
        let ret = ChaChaCtx::Aead {
            cipher,
            nonce,
            tag: [0u8; CHACHA_TAG_LEN],
            already_called: false,
        };
        Ok(ret)
    }
}

/// Stream ciphers are magic, you apply the same operation to encrypt an unencrypted buffer or to
/// decrypt an encrypted one.
fn stream_apply_ks(
    cipher: &mut ChaCha20,
    src: &[u8],
    mut dst: &mut [u8],
) -> Result<usize, EncryptionError> {
    if dst.len() > src.len() {
        dst = &mut dst[..src.len()];
    }
    cipher.apply_keystream_b2b(src, dst).map_err(|e| {
        error!("encryption error: {e}");
        EncryptionError::Other
    })?;
    Ok(src.len())
}
