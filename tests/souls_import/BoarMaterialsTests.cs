// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
static class BoarMaterialsTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Reject(Action action){try{action();}catch(InvalidDataException){return;}throw new Exception("Unsupported boar material accepted");}
    static byte[] Pixel(byte[] specular,Vector3? tint=null){var bytes=BoarMaterials.Reflectance(SkinRetarget.RgbaTexture(specular),tint??Vector3.One);var h=new DDS(bytes);Check((int)h.GetDXGIFormat()==28,"Legacy F0 was gamma-tagged");return bytes.AsSpan(h.DataOffset,4).ToArray();}
    public static void Main() {
        // Source IF_Z(workflow) makes authored workflow1 take the legacy ELSE:
        // F0=pow(saturate(specularRGB*MTDSpecularTint),2.2), roughness=alpha.
        Check(Pixel([128,128,128,0]).SequenceEqual(new byte[]{56,56,56,255}),"Legacy specular RGB was treated as metallic/specular/AO channels");
        Check(Pixel([255,128,64,255]).SequenceEqual(new byte[]{255,56,12,255}),"Legacy colored reflection channel/gamma differs");
        Check(Pixel([255,255,255,0],new(.5f)).SequenceEqual(new byte[]{55,55,55,255}),"MTD tint was applied after source gamma conversion");
        Check(Pixel([255,255,255,0],new(1.2f)).SequenceEqual(new byte[]{255,255,255,255}),"Source MUL_SAT was omitted");
        Check(Pixel([128,64,255,0]).SequenceEqual(Pixel([128,64,255,255])),"Source roughness alpha incorrectly attenuated reflectance");
        Check(Pixel([0,0,0,255]).SequenceEqual(new byte[]{0,0,0,255}),"Source black reflectance was brightened");
        Check(BoarMaterials.NativeShininess(0)==1&&BoarMaterials.NativeShininess(1)==0,"Smooth/rough native exponent limits changed");
        for(int i=2;i<=8;i++){double r=i/10d,a4=r*r*r*r,h=Math.Sqrt((1-Math.Sqrt(2)*a4)/(1-a4));double n=Math.Pow(2,13*BoarMaterials.NativeShininess((float)r));Check(Math.Abs(Math.Pow(h,n)-.5)<.000001,"Native/source highlight half-width differs");}
        Check(Enumerable.Range(0,256).Select(i=>BoarMaterials.NativeShininess(i/255f)).Zip(Enumerable.Range(1,255).Select(i=>BoarMaterials.NativeShininess(i/255f))).All(p=>p.First>=p.Second),"Native shininess does not decrease with authored roughness");
        byte[] Dds(byte[] data){var h=new DDS{dwWidth=2,dwHeight=1,dwMipMapCount=2,dwPitchOrLinearSize=8};h.dwFlags|=DDS.DDSD.PITCH|DDS.DDSD.MIPMAPCOUNT;h.dwCaps|=DDS.DDSCAPS.COMPLEX|DDS.DDSCAPS.MIPMAP;h.ddspf.dwFlags=DDS.DDPF.RGB|DDS.DDPF.ALPHAPIXELS;h.ddspf.dwRGBBitCount=32;h.ddspf.dwRBitMask=255;h.ddspf.dwGBitMask=65280;h.ddspf.dwBBitMask=16711680;h.ddspf.dwABitMask=4278190080;return h.Write(data);}
        var source=Dds([128,0,255,0,255,128,0,255,64,64,64,128]);var before=source.ToArray();var output=BoarMaterials.Reflectance(source,Vector3.One);var levels=SkinRetarget.DdsMipImages(output)[0].subImages;
        Check(levels.Count==2&&levels[0].SequenceEqual(new byte[]{56,0,255,255,255,56,0,255})&&levels[1].SequenceEqual(new byte[]{12,12,12,255}),"Rectangular source specular mips changed");
        var shine=SkinRetarget.DdsMipImages(BoarMaterials.Shininess(source))[0].subImages;Check(shine[0].SequenceEqual(new byte[]{255,255,255,255,0,0,0,255})&&shine[1][0]==(byte)MathF.Round(255*BoarMaterials.NativeShininess(128/255f)),"Shininess was read from source RGB instead of roughness alpha");
        var texture=SkinRetarget.NativeTexture("c8801_source_f0_test",output);var tpf=new TPF{Platform=TPF.TPFPlatform.PS4,Encoding=1,Flag2=3};tpf.Textures.Add(texture);var read=TPF.Read(tpf.Write()).Textures.Single();Check(SkinRetarget.DdsMipImages(read.HeaderizeExt(out _))[0].subImages.Zip(levels).All(p=>p.First.SequenceEqual(p.Second)),"PS4 F0 pixel/mip readback changed");Check(before.SequenceEqual(source),"Source material input was mutated");
        Reject(()=>BoarMaterials.Reflectance(source,new(float.NaN)));Reject(()=>BoarMaterials.NativeShininess(float.NaN));Reject(()=>BoarMaterials.NativeShininess(-.01f));
        Console.WriteLine("PASS: authored workflow1 legacy RGB/gamma/tint, independent highlight half-width, roughness-alpha isolation, rectangular mips and PS4 readback");
    }
}
