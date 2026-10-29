// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

internal static class MapMaterial {
    internal static bool Cutout(string mtd,string profile)=>profile is "DS1" or "DS1R" &&
        (mtd.Contains("_Edge",StringComparison.OrdinalIgnoreCase)||mtd.Contains("Foliage",StringComparison.OrdinalIgnoreCase)||mtd.Contains("Ivy",StringComparison.OrdinalIgnoreCase));

    internal static bool NonSolidFoliage(string mtd,string diffuse,string profile)=>profile is "DS1" or "DS1R" &&
        (mtd.Contains("Foliage",StringComparison.OrdinalIgnoreCase)||mtd.Contains("Ivy",StringComparison.OrdinalIgnoreCase)||
         diffuse.Contains("_grass_",StringComparison.OrdinalIgnoreCase)||diffuse.Contains("_ivy_",StringComparison.OrdinalIgnoreCase));

    // Authored effect cards need additive rendering and must never form solid collision.
    internal static bool LightShaft(string mtd,string profile)=>profile is "DS1" or "DS1R" &&
        mtd.Contains("BG_shaft",StringComparison.OrdinalIgnoreCase)&&mtd.Contains("_Add",StringComparison.OrdinalIgnoreCase);

    internal static bool SameSlot(string source,string native) {
        string Key(string value)=>value.EndsWith("Texture",StringComparison.Ordinal)?value[..^7]:value;
        return Key(source)==Key(native);
    }

    internal static byte[] Normal(byte[] dds,string profile)=>profile is "DS2" or "DS2S"?Normal(dds):dds;

    // DS2 BC3 normals store X in alpha and Y in green. Bloodborne expects X in
    // red. Preserve the blue surface channel and every mip while repacking.
    internal static byte[] Normal(byte[] dds) {
        var source = new DDS(dds);
        if ((int)source.GetDXGIFormat() != 77) return dds;
        using var image = Pfim.Pfimage.FromStream(new MemoryStream(dds));
        if (image.Format != Pfim.ImageFormat.Rgba32) throw new InvalidDataException("Expected BC3 normal pixels");
        using var pixels = new MemoryStream();
        void Mip(int offset, int width, int height, int stride) {
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    int i = checked(offset + y * stride + x * 4);
                    pixels.WriteByte(image.Data[i + 3]); // Pfim returns BGRA.
                    pixels.WriteByte(image.Data[i + 1]);
                    pixels.WriteByte(image.Data[i]);
                    pixels.WriteByte(255);
                }
        }
        Mip(0, image.Width, image.Height, image.Stride);
        foreach (var mip in image.MipMaps) Mip(mip.DataOffset, mip.Width, mip.Height, mip.Stride);
        var header = new DDS {
            dwWidth = image.Width, dwHeight = image.Height,
            dwMipMapCount = image.MipMaps.Length + 1, dwPitchOrLinearSize = image.Width * 4
        };
        header.dwFlags |= DDS.DDSD.PITCH | DDS.DDSD.MIPMAPCOUNT;
        header.dwCaps |= DDS.DDSCAPS.COMPLEX | DDS.DDSCAPS.MIPMAP;
        header.ddspf.dwFlags = DDS.DDPF.RGB | DDS.DDPF.ALPHAPIXELS;
        header.ddspf.dwRGBBitCount = 32;
        header.ddspf.dwRBitMask = 0xff; header.ddspf.dwGBitMask = 0xff00;
        header.ddspf.dwBBitMask = 0xff0000; header.ddspf.dwABitMask = 0xff000000;
        return header.Write(pixels.ToArray());
    }
}
