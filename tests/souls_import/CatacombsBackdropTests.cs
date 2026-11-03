// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

internal static class CatacombsBackdropTests {
    static void Check(bool ok,string message){if(!ok)throw new InvalidDataException(message);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}throw new InvalidDataException(message);}
    static void Main(string[] args) {
        if(args.Length!=3)throw new ArgumentException("DS1R root, BB dvdroot and output directory required");
        string sourceRoot=args[0],nativeRoot=args[1],output=args[2];Directory.CreateDirectory(output);
        var inputs=new Dictionary<string,string>();byte[] Read(string root,string path){string full=Path.Combine(root,path);var bytes=File.ReadAllBytes(full);inputs[full]=Convert.ToHexString(SHA256.HashData(bytes));return bytes;}
        byte[] Source(string p)=>Read(sourceRoot,p);byte[] Native(string p)=>Read(nativeRoot,p);
        var msb=MSB1.Read(Source("map/MapStudio/m13_00_00_00.msb"));var part=msb.Parts.MapPieces.Single(p=>p.Name=="m9900B0");
        var input=Source("map/m13_00_00_00/m9900B0A13.flver.dcx");
        Check(Convert.ToHexString(SHA256.HashData(input)).Equals("a9677e63e3bed838d4f45ef159874514a82e067a4aded107e431f3f1237b394a",StringComparison.OrdinalIgnoreCase),"Authored Catacombs backdrop fixture input changed");
        var source=SourceFlver.Read(input,"DS1R");var original=source.Write();
        Check(part.ModelName=="m9900B0"&&part.Position==new Vector3(46,-105,182)&&part.Rotation==Vector3.Zero&&part.Scale==Vector3.One,"Authored backdrop placement changed");
        Check(part.DrawGroups.SequenceEqual(new uint[]{6260620,0,0,0}),"Source backdrop visibility groups changed");
        Check(source.Meshes.Count==1&&source.Meshes[0].Vertices.Count==294&&source.Materials.Count==1,"Authored backdrop mesh contract changed");
        Check(source.Meshes[0].FaceSets.All(f=>!f.CullBackfaces)&&source.Meshes[0].Vertices.Any(v=>v.Colors[0].A<1),"Backdrop double-sided/alpha contract changed");
        Check(!MapEnvironment.HasSkyMaterials(source),"Generic alpha MTD unexpectedly became global sky");
        Check(MapEnvironment.HasSkyMaterials(source,"m13_00_00_00","m9900B0","DS1R"),"Exact Catacombs background was not recognized");
        foreach(var context in new[]{("m12_00_00_00","m9900B0","DS1R"),("m13_00_00_00","m9901B0","DS1R"),("m13_00_00_00","m9900B0","DS1")})
            Check(!MapEnvironment.HasSkyMaterials(source,context.Item1,context.Item2,context.Item3),"Backdrop accepted wrong source context");
        var changed=FLVER2.Read(original);changed.Materials[0].Textures.Single(t=>t.ParamName=="g_Diffuse").Path="m13_other.tga";
        Check(!MapEnvironment.HasSkyMaterials(changed,"m13_00_00_00","m9900B0","DS1R"),"Backdrop accepted unrelated alpha texture");
        changed=FLVER2.Read(original);changed.Materials[0].MTD="M[D].mtd";
        Check(!MapEnvironment.HasSkyMaterials(changed,"m13_00_00_00","m9900B0","DS1R"),"Backdrop accepted another MTD");
        var sourceMtd=BND3.Read(Source("mtd/Mtd.mtdbnd.dcx"));var nativeMtd=BND4.Read(Native("mtd/allmaterialbnd.mtdbnd.dcx"));
        MTD Definition(IReadOnlyList<BinderFile> files,string path)=>MTD.Read(files.Single(f=>Leaf(f.Name).Equals(Leaf(path),StringComparison.OrdinalIgnoreCase)).Bytes);
        var template=Native("map/m24_01_00_00/m24_01_00_00_900000.flver.dcx");var native=FLVER2.Read(template);var nativeBefore=native.Write();
        // Read real authored pixels and exercise the same sRGB + PS4 conversion
        // used by MapImport.Image, without fabricating a sky texture.
        TPF.Texture? sky=null;
        for(int bank=0;bank<4&&sky==null;bank++) {
            string stem="map/m13/m13_"+bank.ToString("D4");
            using var split=new BXF3Reader(Source(stem+".tpfbhd"),Source(stem+".tpfbdt"));
            foreach(var entry in split.Files.Where(f=>Leaf(f.Name).Equals("m13_sky.tpf.dcx",StringComparison.OrdinalIgnoreCase)))
                sky=TPF.Read(split.ReadFile(entry).ToArray()).Textures.Single(t=>t.Name=="m13_sky");
        }
        Check(sky!=null,"Source m13_sky texture absent");
        var dds=sky!.HeaderizeExt(out var extension);Check(extension==".dds","Source backdrop texture must be DDS");
        var srgb=SkinRetarget.NativeSrgbColor(dds);var texture=SkinRetarget.NativeTexture("m40_import_m13_00_00_00_m13_sky_srgb",srgb);
        var bankOut=new TPF{Platform=TPF.TPFPlatform.PS4,Encoding=1,Flag2=3};bankOut.Textures.Add(texture);var bankBytes=bankOut.Write(DCX.Type.DCX_DFLT_10000_44_9);
        var restored=TPF.Read(bankBytes).Textures.Single();var restoredDds=restored.HeaderizeExt(out extension);
        var beforeImages=SkinRetarget.DdsMipImages(srgb);var afterImages=SkinRetarget.DdsMipImages(restoredDds);
        Check(extension==".dds"&&beforeImages.Count==afterImages.Count,"Backdrop texture changed image count");
        for(int f=0;f<beforeImages.Count;f++)for(int mip=0;mip<beforeImages[f].subImages.Count;mip++)Check(beforeImages[f].subImages[mip].AsSpan().SequenceEqual(afterImages[f].subImages[mip]),"Backdrop native texture changed a source mip block");
        int requests=0;
        EnvironmentSky Bake(FLVER2 model,string? map="m13_00_00_00")=>MapEnvironment.BakeSky(model,native,p=>Definition(sourceMtd.Files,p),p=>Definition(nativeMtd.Files,p),p=>{Check(Leaf(p)=="m13_sky.tga","Unexpected backdrop texture request");requests++;return @"N:\SPRJ\data\Model\map\m40\tex\"+texture.Name+".tif";},"BBImport_m13_00_00_00",sourceMap:map,sourceModel:"m9900B0",sourceProfile:"DS1R");
        Reject(()=>Bake(source,null),"Generic alpha mesh accepted without exact context");
        changed=FLVER2.Read(original);changed.Meshes[0].FaceSets[0].CullBackfaces=true;Reject(()=>Bake(changed),"Changed source backdrop culling accepted");
        changed=FLVER2.Read(original);changed.Meshes[0].Vertices.RemoveAt(0);Reject(()=>Bake(changed),"Changed source backdrop vertex contract accepted");
        var baked=Bake(source);Check(requests==1&&baked.Materials.Count==1&&baked.Model.Meshes.Count==1,"Backdrop emitted duplicate meshes or texture requests");
        var scroll=baked.Model.GXLists[baked.Model.Materials.Single().GXIndex].Single(g=>g.ID=="GX20");Check(scroll.Unk04==100&&scroll.Data.Length==12&&scroll.Data.All(b=>b==0),"Authored static backdrop inherited moving native clouds");
        Check(baked.Limitations.Any(n=>n.Contains("not an invented arena roof",StringComparison.Ordinal)),"Backdrop room-culling limit missing");
        var encoded=baked.Model.Write(DCX.Type.DCX_DFLT_10000_44_9);var round=FLVER2.Read(encoded);var mesh=round.Meshes.Single();var geometry=new StaticMapGeometry(source,"DS1R");
        Check(mesh.Vertices.Count==294&&!mesh.UseBoneWeights&&mesh.NodeIndex==0&&round.Nodes.Count==1,"Backdrop native rigid binding changed");
        var faces=StaticMapGeometry.Faces(source,source.Meshes[0]).ToArray();Check(mesh.FaceSets.Count==faces.Length,"Backdrop lost source face sets");
        for(int f=0;f<faces.Length;f++)Check(!mesh.FaceSets[f].CullBackfaces&&mesh.FaceSets[f].Indices.SequenceEqual(faces[f].Indices),"Backdrop source winding/culling changed");
        for(int n=0;n<mesh.Vertices.Count;n++) {
            var expected=geometry.Bake(source.Meshes[0],source.Meshes[0].Vertices[n],true);var actual=mesh.Vertices[n];
            Check(Vector3.Distance(expected.Position,actual.Position)<.0001f,"Backdrop source geometry changed");
            Check(actual.Colors.Count>0&&Math.Abs(expected.Colors[0].A-actual.Colors[^1].A)<=1f/255+.00001f,"Backdrop authored alpha lost in native layout");
            Check(Vector3.Distance(expected.UVs[0],actual.UVs[0])<.002f,"Backdrop source UV changed");
        }
        var definition=MTD.Read(baked.Materials.Single().Definition.Write());var from=Definition(sourceMtd.Files,source.Materials[0].MTD);
        Check(((float[])definition.Params.Single(p=>p.Name=="g_DiffuseColor").Value).SequenceEqual((float[])from.Params.Single(p=>p.Name=="g_DiffuseMapColor").Value),"Backdrop diffuse RGB multiplier changed");
        Check(Equals(definition.Params.Single(p=>p.Name=="g_DiffuseColorPower").Value,from.Params.Single(p=>p.Name=="g_DiffuseMapColorPower").Value),"Backdrop diffuse gain changed");
        Check(round.Materials.Single().Textures.Single(t=>t.ParamName=="g_DiffuseTexture").Path.Contains(texture.Name,StringComparison.Ordinal),"Backdrop source texture not bound");
        Check(native.Write().AsSpan().SequenceEqual(nativeBefore)&&source.Write().AsSpan().SequenceEqual(original),"Backdrop conversion mutated a source/template model");
        foreach(var (path,hash) in inputs)Check(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)))==hash,"Original resource changed: "+path);
        File.WriteAllBytes(Path.Combine(output,"backdrop.flver.dcx"),encoded);File.WriteAllBytes(Path.Combine(output,"backdrop.tpf.dcx"),bankBytes);File.WriteAllBytes(Path.Combine(output,baked.Materials.Single().Name),definition.Write());
        File.WriteAllText(Path.Combine(output,"report.json"),JsonSerializer.Serialize(new{sourceMap="m13_00_00_00",sourceModel=part.ModelName,part.Position,part.Rotation,part.Scale,part.DrawGroups,part.DispGroups,vertices=mesh.Vertices.Count,triangles=mesh.FaceSets.Sum(f=>f.Indices.Count/3),baked.Limitations,inputs},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS: exact authored Catacombs backdrop; 294 vertices, alpha/culling/UVs, real PS4 texture mip blocks, context/drift negatives; {inputs.Count} original inputs unchanged. Full export visibility and live arena appearance require runtime validation.");
    }
}
