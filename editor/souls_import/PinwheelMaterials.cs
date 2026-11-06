// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

// Pinwheel's torso and cape share the verified legacy-specular workflow used
// by BoarMaterials. Its six mask meshes use the separately verified packed
// workflow with ordinary material override defaults and opaque AO.
internal static class PinwheelMaterials {
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string TextureName(string p)=>Leaf(p).Split('.')[0];
    static readonly HashSet<string> Supported=new(StringComparer.Ordinal) {
        "C_DullLeather[DSB]_Spec.mtd","C_DullLeather[DSB]_Edge_Spec.mtd","C_DullLeather[DSB].mtd"
    };
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Pinwheel material: "+why);}
    internal static Vector3 MaskVertexTint(IReadOnlyList<FLVER.Vertex> vertices) {
        Need(vertices.Count>0&&vertices.All(v=>v.Colors.Count==1),"mask vertex colors unavailable");
        var colors=vertices.Select(v=>v.Colors.Single()).Select(c=>new Vector3(c.R,c.G,c.B)).ToArray();var low=colors.Aggregate(Vector3.Min);var high=colors.Aggregate(Vector3.Max);
        Need(colors.All(v=>new[]{v.X,v.Y,v.Z}.All(c=>float.IsFinite(c)&&c>=0&&c<=1)),"invalid mask vertex tint");
        // The carrier ignores vertex RGB. Preserve uniform inner-face tint;
        // accept only the actual 254..255 near-white outer-face variation.
        Need(low==high||new[]{low.X,low.Y,low.Z}.All(c=>c>=254f/255f-.000001f)&&high==Vector3.One,"unsupported nonuniform mask vertex tint");
        return high;
    }
    internal sealed record PackedResult(byte[] Diffuse,byte[] Reflectance,byte[] Shininess,int OpaqueTexels);
    static byte[] Rgba(int width,int height,IReadOnlyList<byte[]> mips) {
        var h=new DDS{dwWidth=width,dwHeight=height,dwMipMapCount=mips.Count,dwPitchOrLinearSize=width*4};h.dwFlags|=DDS.DDSD.PITCH;
        if(mips.Count>1){h.dwFlags|=DDS.DDSD.MIPMAPCOUNT;h.dwCaps|=DDS.DDSCAPS.COMPLEX|DDS.DDSCAPS.MIPMAP;}
        h.ddspf.dwFlags=DDS.DDPF.RGB|DDS.DDPF.ALPHAPIXELS;h.ddspf.dwRGBBitCount=32;h.ddspf.dwRBitMask=255;h.ddspf.dwGBitMask=65280;h.ddspf.dwBBitMask=16711680;h.ddspf.dwABitMask=4278190080;return h.Write(mips.SelectMany(b=>b).ToArray());
    }
    internal static PackedResult Packed(byte[] diffuse,byte[] packed,Vector3 diffuseTint,Vector3 specularTint,Vector3 vertexTint) {
        Need(new[]{diffuseTint,specularTint,vertexTint}.All(v=>new[]{v.X,v.Y,v.Z}.All(c=>float.IsFinite(c)&&c>=0&&c<=2)),"invalid authored packed tint");
        var d=new DDS(diffuse);var s=new DDS(packed);Need(d.dwWidth==s.dwWidth&&d.dwHeight==s.dwHeight&&d.dwWidth is >=1 and <=1024&&d.dwHeight is >=1 and <=1024,"packed mask dimensions changed");
        var dm=SkinRetarget.DdsMipImages(diffuse)[0].subImages;var sm=SkinRetarget.DdsMipImages(packed)[0].subImages;Need(dm.Count==sm.Count,"packed mask mip ownership changed");
        var albedo=new List<byte[]>();var f0=new List<byte[]>();var gloss=new List<byte[]>();int opaque=0;
        static byte Quantize(float value)=>(byte)Math.Clamp((int)MathF.Round(255*value),0,255);
        for(int mip=0;mip<dm.Count;mip++) {
            int w=Math.Max(1,d.dwWidth>>mip),h=Math.Max(1,d.dwHeight>>mip);using var di=Pfim.Pfimage.FromStream(new MemoryStream(new DDS(diffuse){dwWidth=w,dwHeight=h,dwMipMapCount=1}.Write(dm[mip])));using var si=Pfim.Pfimage.FromStream(new MemoryStream(new DDS(packed){dwWidth=w,dwHeight=h,dwMipMapCount=1}.Write(sm[mip])));
            Need(di.Format==Pfim.ImageFormat.Rgba32&&si.Format==Pfim.ImageFormat.Rgba32,"packed mask RGBA decode required");
            var a=new byte[w*h*4];var r=new byte[a.Length];var g=new byte[a.Length];
            for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
                int at=y*di.Stride+x*4,st=y*si.Stride+x*4,o=(y*w+x)*4;
                // Source default override=(0,0,0,1). Its extra albedo term
                // is (1-AO)*10; every authored mask mip must prove AO=1.
                Need(si.Data[st+3]==255&&di.Data[at+3]==255,"nonopaque source mask requires the additional occlusion/alpha shader path");opaque++;
                float metallic=si.Data[st+1]/255f,roughness=si.Data[st+2]/255f;
                float dielectric=Math.Clamp(si.Data[st]/255f*.2f*Math.Max(Vector3.Dot(specularTint,new(.212673f,.715152f,.072175f)),.001f),0,1);
                var tint=Vector3.Lerp(diffuseTint,specularTint,metallic);var product=new Vector3(di.Data[at+2],di.Data[at+1],di.Data[at])/255f*vertexTint*tint;
                var linear=new Vector3(MathF.Pow(Math.Abs(product.X),2.2f),MathF.Pow(Math.Abs(product.Y),2.2f),MathF.Pow(Math.Abs(product.Z),2.2f));
                var sourceDiffuse=linear*(1-metallic);var sourceF0=Vector3.Clamp(Vector3.Lerp(new(dielectric),linear,metallic),Vector3.Zero,Vector3.One);
                a[o]=Quantize(sourceDiffuse.X);a[o+1]=Quantize(sourceDiffuse.Y);a[o+2]=Quantize(sourceDiffuse.Z);a[o+3]=255;
                r[o]=Quantize(sourceF0.X);r[o+1]=Quantize(sourceF0.Y);r[o+2]=Quantize(sourceF0.Z);r[o+3]=255;
                g[o]=g[o+1]=g[o+2]=Quantize(BoarMaterials.NativeShininess(roughness));g[o+3]=255;
            }
            albedo.Add(a);f0.Add(r);gloss.Add(g);
        }
        return new(Rgba(d.dwWidth,d.dwHeight,albedo),Rgba(d.dwWidth,d.dwHeight,f0),Rgba(d.dwWidth,d.dwHeight,gloss),opaque);
    }
    internal static void Apply(Func<string,byte[]?> sourceRead,byte[] sourceModel,IReadOnlyList<TPF.Texture> textures,SkinBake bake,string target) {
        var source=FLVER2.Read(sourceModel);
        Need(target==PinwheelCharacters.Model&&source.Meshes.Count==13&&source.Materials.Count==8&&bake.Model.Meshes.Count==13&&bake.Model.Materials.Count==13,"authored c3320 population required");
        var definitions=new Dictionary<string,MTD>(StringComparer.Ordinal);
        foreach(string path in new[]{"mtd/Mtd.mtdbnd.dcx","mtd/MtdPatch.mtdbnd.dcx"}) {
            var bytes=sourceRead(path)??throw new FileNotFoundException("Source Pinwheel material definitions unavailable",path);
            foreach(var f in BND3.Read(bytes).Files)if(Supported.Contains(Leaf(f.Name))) {
                string expected=Leaf(f.Name) switch {
                    "C_DullLeather[DSB].mtd"=>"613a3296e7906b2dea5c0cd48bda37c8628b850bad9988688a40595c4c49eaad",
                    "C_DullLeather[DSB]_Edge_Spec.mtd"=>"16be2b79e4b2b0c1efcbc45ee32a49ab535faae873d621b4316b37170f280d11",
                    _=>"3943230db12bd7d03295227d700a1d448f85c15d679a2be56522a44b6b195ae9"};
                Need(Convert.ToHexStringLower(SHA256.HashData(f.Bytes.Span))==expected,"authored material definition changed");
                var m=MTD.Read(f.Bytes);Need(!m.Params.Any(p=>p.Name.StartsWith("g_GhostOverride",StringComparison.Ordinal)),"runtime ghost overrides unsupported");definitions[Leaf(f.Name)]=m;
            }
        }
        var selected=source.Meshes.Select((m,i)=>(Material:source.Materials[m.MaterialIndex],Index:i)).Where(p=>Leaf(p.Material.MTD)!="C_DullLeather[DSB].mtd").ToArray();
        Need(selected.Select(p=>p.Index).SequenceEqual(Enumerable.Range(0,7)),"source torso/cape material ownership changed");
        var packed=textures.Single(t=>t.Name=="c3320_s");string reflectance=target+"_pinwheel_legacy_f0",shininess=target+"_pinwheel_legacy_shine";
        Need(!bake.Textures.Textures.Any(t=>t.Name==reflectance||t.Name==shininess),"owned material textures already present");
        Vector3? specularTint=null;int edgeCount=0;
        foreach(var (material,index) in selected) {
            Need(TextureName(material.Textures.Single(t=>t.ParamName=="g_Specular").Path)==packed.Name,"source legacy specular texture changed");
            Need(definitions.TryGetValue(Leaf(material.MTD),out var mtd),"source legacy definition missing");
            object Value(string key)=>mtd!.Params.Single(p=>p.Name==key).Value;
            Need(Leaf(mtd!.ShaderPath)=="FRPG_Phn_ColDifSpcBmp.spx"&&Convert.ToInt32(Value("g_MaterialWorkflow"))==1&&Convert.ToSingle(Value("g_SpecularPower"))==1,"source legacy shader/workflow changed");
            var rgb=(float[])Value("g_SpecularMapColor");Need(rgb.Length==3,"source specular tint shape changed");var tint=new Vector3(rgb[0],rgb[1],rgb[2])*Convert.ToSingle(Value("g_SpecularMapColorPower"));
            Need(specularTint==null||specularTint==tint,"source legacy material tints diverged");specularTint=tint;
            int blend=Convert.ToInt32(Value("g_BlendMode"));Need(blend==(Leaf(material.MTD)=="C_DullLeather[DSB]_Edge_Spec.mtd"?1:0),"source alpha-test contract changed");
            var destination=bake.Model.Materials[index];Need(Leaf(destination.MTD)=="C[ARSN].mtd","unexpected native carrier material");
            destination.Textures.Single(t=>t.ParamName=="g_SpecularTexture").Path=@"N:\SPRJ\data\Model\chr\"+target+@"\tex\"+reflectance+".tif";
            destination.Textures.Single(t=>t.ParamName=="g_ShininessTexture").Path=@"N:\SPRJ\data\Model\chr\"+target+@"\tex\"+shininess+".tif";
            if(blend==1){destination.MTD=destination.MTD[..^4]+"_e.mtd";edgeCount++;}
        }
        Need(specularTint.HasValue&&edgeCount==2,"source legacy/edge coverage changed");
        var dds=packed.HeaderizeExt(out var extension);Need(extension==".dds","source legacy texture is not DDS");
        bake.Textures.Textures.Add(SkinRetarget.NativeTexture(reflectance,BoarMaterials.Reflectance(dds,specularTint.Value)));
        bake.Textures.Textures.Add(SkinRetarget.NativeTexture(shininess,BoarMaterials.Shininess(dds)));
        var mask=definitions["C_DullLeather[DSB].mtd"];object MaskValue(string key)=>mask.Params.Single(p=>p.Name==key).Value;
        Need(Leaf(mask.ShaderPath)=="FRPG_Phn_ColDifSpcBmp.spx"&&Convert.ToInt32(MaskValue("g_MaterialWorkflow"))==0&&Convert.ToInt32(MaskValue("g_BlendMode"))==0,"source packed mask workflow changed");
        Vector3 MaskTint(string key){var rgb=(float[])MaskValue(key+"Color");Need(rgb.Length==3,"source mask tint shape changed");return new Vector3(rgb[0],rgb[1],rgb[2])*Convert.ToSingle(MaskValue(key+"ColorPower"));}
        var available=textures.ToDictionary(t=>t.Name,StringComparer.Ordinal);
        for(int mesh=7;mesh<13;mesh++) {
            var original=source.Materials[source.Meshes[mesh].MaterialIndex];Need(Leaf(original.MTD)=="C_DullLeather[DSB].mtd","source mask ownership changed");
            var vertexTint=MaskVertexTint(source.Meshes[mesh].Vertices);
            string diffuseName=TextureName(original.Textures.Single(t=>t.ParamName=="g_Diffuse").Path),specularName=TextureName(original.Textures.Single(t=>t.ParamName=="g_Specular").Path);
            Need(diffuseName=="c3320_HD_A_"+(3320+(mesh-7)%3)&&specularName==diffuseName+"_s","source mask texture ownership changed");
            var converted=Packed(available[diffuseName].HeaderizeExt(out _),available[specularName].HeaderizeExt(out _),MaskTint("g_DiffuseMap"),MaskTint("g_SpecularMap"),vertexTint);
            var destination=bake.Model.Materials[mesh];Need(Leaf(destination.MTD)=="C[ARSN].mtd","unexpected mask native material");
            foreach(var (slot,suffix,bytes) in new[]{("g_DiffuseTexture","diffuse",converted.Diffuse),("g_SpecularTexture","f0",converted.Reflectance),("g_ShininessTexture","shine",converted.Shininess)}) {
                string name=target+"_pinwheel_mask_"+mesh+"_"+suffix;Need(!bake.Textures.Textures.Any(t=>t.Name==name),"owned mask texture collision");bake.Textures.Textures.Add(SkinRetarget.NativeTexture(name,bytes));
                destination.Textures.Single(t=>t.ParamName==slot).Path=@"N:\SPRJ\data\Model\chr\"+target+@"\tex\"+name+".tif";
            }
        }
        int replaced=0;for(int i=0;i<bake.TextureFallbacks.Count&&replaced<13;) {
            if(bake.TextureFallbacks[i].StartsWith("Standard_1: DS1R packed g_Specular -> shininess",StringComparison.Ordinal)){bake.TextureFallbacks.RemoveAt(i);replaced++;}else i++;
        }
        Need(replaced==13,"source legacy fallback report changed");
        bake.TextureFallbacks.Add("c3320 torso/cape: verified workflow1 RGB specular with authored tint/gamma and alpha roughness converted through the existing native highlight approximation; both edge materials alpha-test. Six mask meshes use source packed metallic/F0/roughness and ordinary override defaults; source opaque AO1 is required. Uniform inner vertex tint is retained before gamma; white/254 outer variation is bounded to0.87% linear energy. Native BRDF/interpolation, incident diffuse light/GI and source room lightmaps remain separate limitations.");
    }
}
