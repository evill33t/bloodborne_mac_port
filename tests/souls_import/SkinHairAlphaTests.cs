// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class SkinHairAlphaTests {
    static void Check(bool condition,string message){if(!condition)throw new Exception(message);}
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static string TextureName(string path)=>Leaf(path).Split('.')[0];
    static byte[][] Pixels(TPF.Texture texture) {var bytes=texture.HeaderizeExt(out var ext);Check(ext==".dds","Hair texture stopped being DDS");return Headerizer.GetDDSTextureBuffers(new DDS(bytes),bytes).SelectMany(i=>i.subImages).ToArray();}
    static void Images(TPF.Texture before,TPF.Texture after) {
        var source=before.HeaderizeExt(out _);var output=after.HeaderizeExt(out _);var a=Pixels(before);var b=Pixels(after);
        if(new DDS(source).GetDXGIFormat()==DDS.DXGI_FORMAT.BC5_UNORM&&new DDS(output).GetDXGIFormat()==DDS.DXGI_FORMAT.R8G8B8A8_UNORM) {
            Check(a.Length==b.Length,"Expanded normal mip count changed");var header=new DDS(source);int width=header.dwWidth,height=header.dwHeight;
            for(int mip=0;mip<a.Length;mip++) {
                var dds=new DDS{dwWidth=width,dwHeight=height,dwMipMapCount=1,dwPitchOrLinearSize=a[mip].Length};dds.ddspf.dwFlags=DDS.DDPF.FOURCC;dds.ddspf.dwFourCC="ATI2";
                using var image=Pfim.Pfimage.FromStream(new MemoryStream(dds.Write(a[mip])));Check(image.Format==Pfim.ImageFormat.Rgb24,"Independent BC5 decoder format changed");
                Check(b[mip].Length==width*height*4,"Expanded normal dimensions changed");
                for(int y=0;y<height;y++)for(int x=0;x<width;x++) {int dest=(y*width+x)*4,src=y*image.Stride+x*3;Check(Math.Abs(b[mip][dest]-image.Data[src+2])<=1&&Math.Abs(b[mip][dest+1]-image.Data[src+1])<=1&&b[mip][dest+2]==255&&b[mip][dest+3]==255,"BC5 RG or neutral gloss/alpha differs from independent decoder");}
                width=Math.Max(1,width/2);height=Math.Max(1,height/2);
            }
        } else Check(a.Length==b.Length&&a.Zip(b).All(p=>p.First.SequenceEqual(p.Second)),"Hair compressed image/alpha/mip payload changed");
    }
    static string Json(object value)=>JsonSerializer.Serialize(value);
    static string Expected(string sourceMtd)=>Leaf(sourceMtd).ToLowerInvariant() switch {"ps_hair[ds]_edge.mtd"=>"C[ARSN]_e.mtd","ps_hair[ds]_alp.mtd"=>"C[ARSN]_al.mtd",_=>"C[ARSN].mtd"};
    static List<TPF.Texture> Textures(List<BinderFile> files)=>files.Where(f=>TPF.Is(f.Bytes.ToArray())).SelectMany(f=>TPF.Read(f.Bytes).Textures).ToList();
    public static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1];var hashes=new Dictionary<string,byte[]>();
        byte[] Read(string root,string relative) {string path=Path.Combine(root,relative);byte[] bytes=File.ReadAllBytes(path);hashes.TryAdd(path,SHA256.HashData(bytes));return bytes;}
        var catalog=BND4.Read(Read(nativeRoot,"mtd/allmaterialbnd.mtdbnd.dcx"));
        MTD Mtd(string name)=>MTD.Read(catalog.Files.Single(f=>Leaf(f.Name)==name).Bytes);
        var opaque=Mtd("C[ARSN].mtd");var cutout=Mtd("C[ARSN]_e.mtd");var blend=Mtd("C[ARSN]_al.mtd");
        Check(opaque.ShaderPath==cutout.ShaderPath&&opaque.ShaderPath==blend.ShaderPath,"Hair alpha variants use incompatible native shader input contracts");
        Check(Json(opaque.Textures)==Json(cutout.Textures)&&Json(opaque.Textures)==Json(blend.Textures),"Hair alpha variants require different texture slots/UV inputs");
        // Both alpha variants additionally spell out the disabled adaptive
        // tessellation flag. This MTD constant does not add a vertex/GX input.
        foreach(var variant in new[]{cutout,blend}) {
            Check(opaque.Params.All(p=>variant.Params.Any(v=>v.Name==p.Name)),"Hair alpha variant lacks a native shader constant");
            var extra=variant.Params.Where(p=>opaque.Params.All(o=>o.Name!=p.Name)).ToArray();
            Check(extra.Length==1&&extra[0].Name=="g_AdaptiveTessellationEnable"&&Json(extra[0].Value)=="0","Hair alpha variant enables an unsupported tessellation input");
        }
        string[] Differences(MTD variant)=>opaque.Params.Where(p=>Json(p.Value)!=Json(variant.Params.Single(v=>v.Name==p.Name).Value)).Select(p=>p.Name).ToArray();
        string[] cutoutDiff=Differences(cutout),blendDiff=Differences(blend);
        Check(cutoutDiff.SequenceEqual(new[]{"g_AlphaRef"}),"Native cutout changed shared fields other than alpha testing");
        Check(blendDiff.SequenceEqual(new[]{"g_BlendMode"}),"Native translucent material changed shared fields other than alpha blending");
        object Value(MTD material,string name)=>material.Params.Single(p=>p.Name==name).Value;
        Check(Json(Value(cutout,"g_AlphaRef"))=="[50,0]"&&Json(Value(cutout,"g_BlendMode"))=="0"&&Json(Value(blend,"g_BlendMode"))=="2","Native hair alpha policy values differ");
        var native=BND4.Read(Read(nativeRoot,"chr/c8030.chrbnd.dcx"));byte[] nativeBytes=native.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();var nativeModel=FLVER2.Read(nativeBytes);
        var template=nativeModel.Meshes.First(m=>m.UseBoneWeights&&Leaf(nativeModel.Materials[m.MaterialIndex].MTD)=="C[ARSN].mtd");var templateMaterial=nativeModel.Materials[template.MaterialIndex];
        var nativeCutouts=nativeModel.Meshes.Where(m=>m.UseBoneWeights&&Leaf(nativeModel.Materials[m.MaterialIndex].MTD)=="C[ARSN]_e.mtd").ToArray();
        Check(nativeCutouts.Length>0&&nativeCutouts.All(m=>nativeModel.Materials[m.MaterialIndex].GXIndex==templateMaterial.GXIndex),"Native Doll cutout material uses a different GX contract");
        Check(nativeCutouts.All(m=>Json(m.VertexBuffers.Select(b=>nativeModel.BufferLayouts[b.LayoutIndex]).ToArray())==Json(template.VertexBuffers.Select(b=>nativeModel.BufferLayouts[b.LayoutIndex]).ToArray())),"Native Doll cutout hair uses different vertex inputs");
        // A synthetic source verifies exact MTD semantics, texture alpha and
        // retained vertex alpha even when misleading material labels disagree.
        var source=FLVER2.Read(nativeBytes);source.Materials=[new FLVER2.Material("Ps_Hair[DS]_Alp",@"N:\FRPG\mtd\Ps_Hair[DS]_Edge.mtd",0){Textures=[new FLVER2.Texture{ParamName="g_Diffuse",Path=@"N:\hair.tga",TilingScale=Vector2.One}]}];
        source.Meshes.Clear();var mesh=new FLVER2.Mesh{UseBoneWeights=true,MaterialIndex=0,VertexBuffers=template.VertexBuffers.Select(b=>new FLVER2.VertexBuffer(b.LayoutIndex){BufferIndex=b.BufferIndex}).ToList()};
        foreach(var p in new[]{new Vector3(0,1,0),new Vector3(.1f,1,0),new Vector3(0,1,.1f)}) {var v=new FLVER.Vertex{Position=p,Normal=Vector3.UnitY,UVs=[Vector3.Zero,Vector3.Zero],Tangents=[new(1,0,0,1),new(1,0,0,1)],Colors=[new(.5f,1f,1f,1f),new(.5f,1f,1f,1f)]};v.BoneIndices[0]=0;v.BoneWeights[0]=1;mesh.Vertices.Add(v);}
        mesh.FaceSets=[new(FLVER2.FaceSet.FSFlags.None,false,false,0,[0,1,2])];source.Meshes.Add(mesh);
        var dds=new DDS{dwWidth=2,dwHeight=2,dwMipMapCount=1,dwPitchOrLinearSize=8};dds.ddspf.dwFlags=DDS.DDPF.RGB|DDS.DDPF.ALPHAPIXELS;dds.ddspf.dwRGBBitCount=32;dds.ddspf.dwRBitMask=0xff;dds.ddspf.dwGBitMask=0xff00;dds.ddspf.dwBBitMask=0xff0000;dds.ddspf.dwABitMask=0xff000000;
        byte[] image=[20,40,60,0,20,40,60,64,20,40,60,128,20,40,60,255];var texture=new TPF.Texture("hair",9,0,dds.Write(image),TPF.TPFPlatform.PC);
        var cases=new[]{("Ps_Hair[DS]_Edge.mtd","C[ARSN]_e.mtd"),("Ps_Hair[DS]_Alp.mtd","C[ARSN]_al.mtd"),("Ps_Hair[DS].mtd","C[ARSN].mtd"),("P_DullLeather[DSB]_Edge.mtd","C[ARSN].mtd"),("C_DullLeather[DSB]_Alp.mtd","C[ARSN].mtd"),("ps_hair[ds]_EDGE.MTD","C[ARSN]_e.mtd")};
        foreach(var (from,to) in cases) {
            source.Materials[0].MTD=@"N:\FRPG\mtd\"+from;byte[] before=source.Write();var result=SkinRetarget.Bake(before,nativeBytes,[texture],textureOwner:"c9800");var model=FLVER2.Read(result.Model.Write());var material=model.Materials.Single();
            Check(Leaf(material.MTD)==to,"Source MTD alpha mode was ignored or an unrelated material policy changed: "+from);
            Check(material.GXIndex==templateMaterial.GXIndex&&material.Index==templateMaterial.Index&&model.GXLists.Count==nativeModel.GXLists.Count,"Hair alpha selection changed native GX bindings");
            Check(model.Meshes.Single().VertexBuffers.Select(v=>v.LayoutIndex).SequenceEqual(template.VertexBuffers.Select(v=>v.LayoutIndex)),"Hair alpha selection changed native vertex layout");
            Check(Json(model.BufferLayouts)==Json(nativeModel.BufferLayouts)&&Json(model.GXLists)==Json(nativeModel.GXLists),"Hair alpha selection modified native layout/GX definitions");
            var readback=TPF.Read(result.Textures.Write()).Textures.Single(t=>t.Name=="c9800_import_hair");Images(texture,readback);Check(Pixels(readback).Single().SequenceEqual(image),"Alpha values [0,64,128,255] changed in PS4 readback");
            Check(model.Meshes[0].Vertices.All(v=>v.Colors.Count>0&&Math.Abs(v.Colors[0].A-.5f)<.01f),"Source vertex alpha changed");
            Check(before.SequenceEqual(source.Write()),"Source FLVER changed during alpha selection");
        }
        // Source profiles must distinguish packed legacy shininess from native
        // RGB reflectance without changing original DS1 or unspecified imports.
        byte[] Block(byte start,byte end) {var data=new byte[8];data[0]=start;data[1]=end;ulong selectors=0;for(int i=0;i<16;i++)selectors|=(ulong)(i%8)<<(3*i);for(int i=0;i<6;i++)data[i+2]=(byte)(selectors>>(8*i));return data;}
        byte[] bc5Pixels=Block(200,20).Concat(Block(30,210)).Concat(Block(150,60)).Concat(Block(190,50)).ToArray();
        var bc5Header=new DDS{dwWidth=4,dwHeight=4,dwMipMapCount=2,dwPitchOrLinearSize=16};bc5Header.ddspf.dwFlags=DDS.DDPF.FOURCC;bc5Header.ddspf.dwFourCC="ATI2";bc5Header.dwFlags|=DDS.DDSD.MIPMAPCOUNT;bc5Header.dwCaps|=DDS.DDSCAPS.MIPMAP|DDS.DDSCAPS.COMPLEX;
        var normal=new TPF.Texture("legacy_n",104,0,bc5Header.Write(bc5Pixels),TPF.TPFPlatform.PC);var packed=new TPF.Texture("legacy_s",9,0,SkinRetarget.RgbaTexture([200,0,64,255]),TPF.TPFPlatform.PC);
        source.Materials[0].Name="Legacy packed surface";source.Materials[0].MTD=@"N:\FRPG\P_Metal[DSB].mtd";
        source.Materials[0].Textures.Add(new FLVER2.Texture{ParamName="g_Specular",Path="legacy_s.tga"});source.Materials[0].Textures.Add(new FLVER2.Texture{ParamName="g_Bumpmap",Path="legacy_n.tga"});
        byte[] legacy=source.Write();var baseline=SkinRetarget.Bake(legacy,nativeBytes,[texture,packed,normal],textureOwner:"c9800");
        foreach(string? profile in new string?[]{null,"DS1","BB","DS3"}) {var old=SkinRetarget.Bake(legacy,nativeBytes,[texture,packed,normal],textureOwner:"c9800",sourceProfile:profile);Check(old.Model.Write().SequenceEqual(baseline.Model.Write())&&old.Textures.Write().SequenceEqual(baseline.Textures.Write()),"An unrelated profile/default import changed material policy");}
        foreach(string profile in new[]{"DS1R","DS2","DS2S"}) {
            var converted=SkinRetarget.Bake(legacy,nativeBytes,[texture,packed,normal],textureOwner:"c9800",sourceProfile:profile);var mat=FLVER2.Read(converted.Model.Write()).Materials.Single();var bank=TPF.Read(converted.Textures.Write());
            Check(mat.Textures.Single(t=>t.ParamName=="g_SpecularTexture").Path==@"N:\SPRJ\data\Other\SysTex\SYSTEX_DummyReflectance.tga","Packed shininess was reused as colored reflectance");
            Check(TextureName(mat.Textures.Single(t=>t.ParamName=="g_ShininessTexture").Path)=="c9800_import_legacy_s","Packed source g_Specular was not mapped to native shininess");
            Images(packed,bank.Textures.Single(t=>t.Name=="c9800_import_legacy_s"));Images(normal,bank.Textures.Single(t=>t.Name=="c9800_import_legacy_n"));
            Check(converted.TextureFallbacks.Any(s=>s.Contains("neutral reflectance approximation"))&&converted.TextureFallbacks.Any(s=>s.Contains("BC5 normal RG retained")),"Native shading approximation is not reported");
            Check(converted.Model.Meshes[0].Vertices.Select(v=>v.Colors[0]).Select(c=>Json(c)).SequenceEqual(baseline.Model.Meshes[0].Vertices.Select(v=>v.Colors[0]).Select(c=>Json(c))),"Unproven vertex mask policy was changed");
        }
        // Odd final blocks exercise cropped edge pixels independently of PS4 tiling.
        var croppedHeader=new DDS{dwWidth=3,dwHeight=2,dwMipMapCount=1,dwPitchOrLinearSize=16};croppedHeader.ddspf.dwFlags=DDS.DDPF.FOURCC;croppedHeader.ddspf.dwFourCC="ATI2";var cropped=new TPF.Texture("crop",104,0,croppedHeader.Write(bc5Pixels[..16]),TPF.TPFPlatform.PC);
        var cropBytes=SkinRetarget.NativeBc5Normal(cropped.Bytes);var cropPixels=cropBytes.AsSpan(new DDS(cropBytes).DataOffset).ToArray();
        Check(cropPixels.Length==24,"Cropped normal dimensions changed");int[] cropR=[200,20,174,122,97,71],cropG=[30,210,66,138,174,0];
        for(int i=0;i<6;i++)Check(cropPixels[4*i]==cropR[i]&&cropPixels[4*i+1]==cropG[i]&&cropPixels[4*i+2]==255&&cropPixels[4*i+3]==255,"Cropped BC5 endpoint/interpolation table changed");
        bool rejected=false;try{SkinRetarget.NativeBc5Normal(SkinRetarget.RgbaTexture([128,128,255,255]));}catch(InvalidDataException){rejected=true;}Check(rejected,"Non-BC5 input accepted by normal expansion");
        var actual=new List<object>();
        foreach(string path in new[]{"parts/HR_M_0003.partsbnd.dcx","parts/HR_M_0011.partsbnd.dcx","chr/c2750.chrbnd.dcx"}) {
            var bank=BND3.Read(Read(sourceRoot,path));byte[] sourceBytes=bank.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();var model=FLVER2.Read(sourceBytes);var textures=Textures(bank.Files);
            if(path.StartsWith("chr/",StringComparison.Ordinal)) {
                using var split=new BXF3Reader(bank.Files.Single(f=>Leaf(f.Name).EndsWith(".chrtpfbhd")).Bytes,Read(sourceRoot,"chr/c2750.chrtpfbdt"));
                foreach(var file in split.Files)textures.AddRange(TPF.Read(split.ReadFile(file)).Textures);
                var names=textures.Select(t=>TextureName(t.Name)).ToHashSet(StringComparer.OrdinalIgnoreCase);
                foreach(string common in new[]{"parts/Common_Body.tpf","parts/Common_Body_M.tpf"})foreach(var commonTexture in TPF.Read(Read(sourceRoot,common)).Textures)if(names.Add(TextureName(commonTexture.Name)))textures.Add(commonTexture);
            }
            var bake=SkinRetarget.Bake(sourceBytes,nativeBytes,textures,textureOwner:"c9801",sourceProfile:"DS1R");var back=FLVER2.Read(bake.Model.Write());var tpf=TPF.Read(bake.Textures.Write());
            Check(model.Meshes.Count==back.Meshes.Count&&bake.Vertices==model.Meshes.Sum(m=>m.Vertices.Count),"Real NPC hair geometry was removed");int edge=0,alp=0;
            for(int i=0;i<model.Meshes.Count;++i) {string from=model.Materials[model.Meshes[i].MaterialIndex].MTD;string to=back.Materials[back.Meshes[i].MaterialIndex].MTD;Check(Leaf(to)==Expected(from),"Real NPC hair MTD alpha selection failed");if(Leaf(to)=="C[ARSN]_e.mtd")++edge;if(Leaf(to)=="C[ARSN]_al.mtd")++alp;}
            Check(edge+alp>0,"Real source NPC hair never exercises source alpha behavior");
            Check(path.StartsWith("chr/",StringComparison.Ordinal)?edge>0:alp>0,"Actual source NPC hair selected the wrong native alpha mode");
            foreach(var tex in textures.Where(t=>tpf.Textures.Any(o=>o.Name=="c9801_import_"+t.Name)))Images(tex,tpf.Textures.Single(t=>t.Name=="c9801_import_"+tex.Name));
            actual.Add(new{source=path,vertices=bake.Vertices,triangles=bake.Triangles,edgeMeshes=edge,blendMeshes=alp,materials=back.Materials.Select(m=>Leaf(m.MTD)).ToArray()});
        }
        foreach(var (path,hash) in hashes)Check(SHA256.HashData(File.ReadAllBytes(path)).SequenceEqual(hash),"Original source/native game file changed");
        string output=Path.Combine(Directory.GetCurrentDirectory(),"out/map-editor/firelink-validation/skin-hair-alpha-tests.json");Directory.CreateDirectory(Path.GetDirectoryName(output)!);File.WriteAllText(output,JsonSerializer.Serialize(new{shader=opaque.ShaderPath,cutoutDiff,blendDiff,alphaRef=Value(cutout,"g_AlphaRef"),blendMode=Value(blend,"g_BlendMode"),actual,unchangedFiles=hashes.Count,gameplayValidated=false},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"Hair alpha passed: native shader/MTD texture contract, unchanged GX/layouts, six exact alpha cases, profile-aware packed shininess/BC5 normals, unchanged default/DS1/BB/DS3 policy, PS4 alpha/mip payload and three real source NPC hair readbacks; {hashes.Count} original files unchanged. Gameplay remains unverified.");
    }
}
