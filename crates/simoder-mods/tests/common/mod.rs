//! Shared test fixtures for `simoder-mods` integration suites.

#![allow(dead_code)]

use std::borrow::Cow;
use std::fs;
use std::path::Path;

use simoder_core::Tgi;

pub fn target_tgi(instance: u32) -> Tgi {
    Tgi {
        type_id: 0x00B1_B104,
        group: 0x61EF_C000,
        instance,
    }
}

pub fn minimal_valid_png(width: u32, height: u32) -> Vec<u8> {
    let mut buf = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut buf, width, height);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().unwrap();
        let pixels = vec![255u8; (width * height * 4) as usize];
        writer.write_image_data(&pixels).unwrap();
    }
    buf
}

pub fn minimal_valid_glb(include_bin_chunk: bool) -> Vec<u8> {
    let json: &[u8] = if include_bin_chunk {
        br#"{"asset":{"version":"2.0","generator":"simoder-test"},"buffers":[{"byteLength":4}]}"#
    } else {
        br#"{"asset":{"version":"2.0","generator":"simoder-test"}}"#
    };
    gltf::binary::Glb {
        header: gltf::binary::Header {
            magic: *b"glTF",
            version: 2,
            length: 0,
        },
        json: Cow::Borrowed(json),
        bin: include_bin_chunk.then_some(Cow::Borrowed(&[0xAA, 0xBB, 0xCC, 0xDD])),
    }
    .to_vec()
    .unwrap()
}

pub fn write_authoring_patch_mod(dir: &Path, id: &str, name: &str, cost_value: f32) {
    fs::create_dir_all(dir).unwrap();
    let content = format!(
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"{id}\"\nname = \"{name}\"\nversion = \"1.0.0\"\nauthor = \"tester\"\n\n\
         [[patches]]\n\
         target = {{ type = \"0x00B1B104\", group = \"0x61EFC000\", instance = \"0x719436BD\" }}\n\
         [[patches.properties]]\n\
         id = \"0x09AE19D7\"\ntype = \"float\"\noperation = \"set\"\nvalue = {cost_value}\n"
    );
    fs::write(dir.join("mod.toml"), content).unwrap();
}
