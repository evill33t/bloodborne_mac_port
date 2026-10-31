// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

// Bounded DS1R armored-boar material adapter. The shipped shader branches
// IF_Z on workflow: authored workflow1 uses legacy RGB specular/alpha roughness.
internal static class BoarMaterials {
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string TextureName(string p)=>Leaf(p).Split('.')[0];
    static readonly HashSet<string> Materials=new(StringComparer.Ordinal) {
        "C_3460_Metal[DSB].mtd","C_Metal[DSB]_Edge_Spec.mtd","C_DullLeather[DSB]_Edge_Spec.mtd","C_Metal[DSB]_Spec.mtd","C_DullLeather[DSB]_Spec.mtd"
    };
    static byte[] Pixels(byte[] dds,int width,int height) {
        using var image=Pfim.Pfimage.FromStream(new MemoryStream(dds));
        if(image.Width!=width||image.Height!=height||image.Format!=Pfim.ImageFormat.Rgba32)throw new InvalidDataException("Boar material requires RGBA source pixels");
        var rgba=new byte[checked(width*height*4)];
        for(int y=0;y<height;y++)for(int x=0;x<width;x++){int i=y*image.Stride+x*4,o=(y*width+x)*4;rgba[o]=image.Data[i+2];rgba[o+1]=image.Data[i+1];rgba[o+2]=image.Data[i];rgba[o+3]=image.Data[i+3];}
        return rgba;
    }
    static byte[] ConvertTexture(byte[] source,Func<byte[],byte[]> convert) {
        var h=new DDS(source);
        if(h.dwWidth is <1 or >2048||h.dwHeight is <1 or >2048)throw new InvalidDataException("Boar material dimensions exceed the supported contract");
        var levels=SkinRetarget.DdsMipImages(source)[0].subImages;using var output=new MemoryStream();
        for(int level=0;level<levels.Count;level++) {
            int width=Math.Max(1,h.dwWidth>>level),height=Math.Max(1,h.dwHeight>>level);
            var header=new DDS(source){dwWidth=width,dwHeight=height,dwMipMapCount=1};output.Write(convert(Pixels(header.Write(levels[level]),width,height)));
        }
        var result=new DDS{dwWidth=h.dwWidth,dwHeight=h.dwHeight,dwMipMapCount=levels.Count,dwPitchOrLinearSize=h.dwWidth*4};
        result.dwFlags|=DDS.DDSD.PITCH;if(levels.Count>1){result.dwFlags|=DDS.DDSD.MIPMAPCOUNT;result.dwCaps|=DDS.DDSCAPS.COMPLEX|DDS.DDSCAPS.MIPMAP;}
        result.ddspf.dwFlags=DDS.DDPF.RGB|DDS.DDPF.ALPHAPIXELS;result.ddspf.dwRGBBitCount=32;result.ddspf.dwRBitMask=0xff;result.ddspf.dwGBitMask=0xff00;result.ddspf.dwBBitMask=0xff0000;result.ddspf.dwABitMask=0xff000000;
        return result.Write(output.ToArray());
    }
    internal static byte[] Reflectance(byte[] specular,Vector3 tint) {
        if(new[]{tint.X,tint.Y,tint.Z}.Any(v=>!float.IsFinite(v)||v<0||v>2))throw new InvalidDataException("Unsupported source boar specular tint");
        // Shipped IF token0x0400001f at09f0 is IF_Z, so workflow1
        // takes ELSE0d40. 0db4..0e10: pow(saturate(specRGB*tint),2.2).
        return ConvertTexture(specular,rgba=> {
            for(int i=0;i<rgba.Length;i+=4) {
                for(int c=0;c<3;c++){float scale=c==0?tint.X:c==1?tint.Y:tint.Z;rgba[i+c]=(byte)Math.Clamp((int)MathF.Round(255*MathF.Pow(Math.Clamp(rgba[i+c]/255f*scale,0,1),2.2f)),0,255);}
                rgba[i+3]=255;
            }
            return rgba;
        });
    }
    internal static float NativeShininess(float roughness) {
        if(!float.IsFinite(roughness)||roughness<0||roughness>1)throw new InvalidDataException("Invalid source boar roughness");
        // Source ELSE0e24..0e7c uses specular alpha as perceptual
        // roughness; GGX1930..1d40 uses alpha=roughness^2. Match D's
        // half-width to native Blinn-Phong n=2^(13*shininess), capped
        // to its native [1,8192] range. This is an explicit BRDF approximation.
        double r=Math.Max(.014,roughness);if(r==1)return 0;
        double a4=r*r*r*r,h2=(1-Math.Sqrt(2)*a4)/(1-a4);if(h2<=0)return 0;
        double n=2*Math.Log(.5)/Math.Log(h2);return(float)Math.Clamp(Math.Log2(n)/13,0,1);
    }
    internal static byte[] Shininess(byte[] specular)=>ConvertTexture(specular,rgba=> {
        for(int i=0;i<rgba.Length;i+=4){byte value=(byte)Math.Clamp((int)MathF.Round(255*NativeShininess(rgba[i+3]/255f)),0,255);rgba[i]=rgba[i+1]=rgba[i+2]=value;rgba[i+3]=255;}return rgba;
    });
    internal static void Apply(Func<string,byte[]?> sourceRead,byte[] sourceModel,IReadOnlyList<TPF.Texture> textures,SkinBake bake,string target) {
        var source=FLVER2.Read(sourceModel);
        if(source.Materials.Count!=7||source.Meshes.Count!=7||bake.Model.Materials.Count!=7||source.Materials.Any(m=>!m.Name.StartsWith("c3460_",StringComparison.Ordinal)||!Materials.Contains(Leaf(m.MTD))))
            throw new InvalidDataException("Boar material adapter requires the authored c3460 mesh/material contract");
        var definitions=new Dictionary<string,MTD>(StringComparer.OrdinalIgnoreCase);
        foreach(string path in new[]{"mtd/Mtd.mtdbnd.dcx","mtd/MtdPatch.mtdbnd.dcx"}) {
            var bytes=sourceRead(path)??throw new FileNotFoundException("Source boar material definitions are unavailable",path);
            foreach(var file in BND3.Read(bytes).Files)if(Materials.Contains(Leaf(file.Name)))definitions[Leaf(file.Name)]=MTD.Read(file.Bytes);
        }
        var available=textures.ToDictionary(t=>t.Name,StringComparer.OrdinalIgnoreCase);var cache=new Dictionary<string,string>(StringComparer.Ordinal);var shininess=new Dictionary<string,string>(StringComparer.Ordinal);
        for(int i=0;i<source.Meshes.Count;i++) {
            var material=source.Materials[source.Meshes[i].MaterialIndex];string name=Leaf(material.MTD);
            if(!definitions.TryGetValue(name,out var mtd))throw new InvalidDataException("Source boar material definition is missing: "+name);
            object Value(string key)=>mtd.Params.Single(p=>p.Name==key).Value;
            Vector3 Tint(string key){var c=(float[])Value(key+"Color");if(c.Length!=3)throw new InvalidDataException("Invalid source boar material tint");return new Vector3(c[0],c[1],c[2])*Convert.ToSingle(Value(key+"ColorPower"));}
            if(Convert.ToInt32(Value("g_MaterialWorkflow"))!=1||!Leaf(mtd.ShaderPath).Equals("FRPG_Phn_ColDifSpcBmp.spx",StringComparison.Ordinal))throw new InvalidDataException("Unsupported source boar material workflow");
            int blend=Convert.ToInt32(Value("g_BlendMode"));if(blend is not(0 or 1))throw new InvalidDataException("Unsupported source boar material blend mode");
            string packedName=TextureName(material.Textures.Single(t=>t.ParamName=="g_Specular").Path);
            if(!available.TryGetValue(packedName,out var packed))throw new InvalidDataException("Source boar specular input is missing");
            if(Convert.ToSingle(Value("g_SpecularPower"))!=1)throw new InvalidDataException("Unsupported source boar specular power");
            var st=Tint("g_SpecularMap");string key=packedName+"/"+st;
            if(!cache.TryGetValue(key,out string? owned)) {
                owned=target+"_source_f0_"+cache.Count;
                if(bake.Textures.Textures.Any(t=>t.Name.Equals(owned,StringComparison.OrdinalIgnoreCase)))throw new InvalidDataException("Owned boar reflectance texture name collision");
                var dds=Reflectance(packed.HeaderizeExt(out _),st);bake.Textures.Textures.Add(SkinRetarget.NativeTexture(owned,dds));cache.Add(key,owned);
            }
            if(!shininess.TryGetValue(packedName,out string? gloss)) {
                gloss=target+"_source_shine_"+shininess.Count;
                if(bake.Textures.Textures.Any(t=>t.Name.Equals(gloss,StringComparison.OrdinalIgnoreCase)))throw new InvalidDataException("Owned boar shininess texture name collision");
                bake.Textures.Textures.Add(SkinRetarget.NativeTexture(gloss,Shininess(packed.HeaderizeExt(out _))));shininess.Add(packedName,gloss);
            }
            var destination=bake.Model.Materials[i];destination.Textures.Single(t=>t.ParamName=="g_SpecularTexture").Path=@"N:\SPRJ\data\Model\chr\"+target+@"\tex\"+owned+".tif";
            destination.Textures.Single(t=>t.ParamName=="g_ShininessTexture").Path=@"N:\SPRJ\data\Model\chr\"+target+@"\tex\"+gloss+".tif";
            if(blend==1)destination.MTD=destination.MTD[..^4]+"_e.mtd";
        }
        bake.TextureFallbacks.RemoveAll(s=>s.Contains(": DS1R packed g_Specular -> shininess",StringComparison.Ordinal));
        bake.TextureFallbacks.Add("c3460: authored workflow1 uses source legacy RGB specular, gamma-decoded with its MTD tint; specular alpha roughness is converted to native shininess by matching highlight half-width. Source edge materials retain alpha testing. Native BRDF, cubemap contents and environment-light response remain approximations.");
    }
}
