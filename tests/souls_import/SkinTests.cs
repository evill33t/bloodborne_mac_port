// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using Type=SoulsFormats.FLVER.LayoutType;
using Semantic=SoulsFormats.FLVER.LayoutSemantic;
static class Tests {
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    static void Near(Vector3 a,Vector3 b){Check(Vector3.Distance(a,b)<.0001f,$"Unexpected vector: {a} != {b}");}
    static void Reject(Action action){bool rejected=false;try{action();}catch{rejected=true;}Check(rejected,"Malformed import was accepted");}
    static FLVER2 Fixture(string child,float height,float angle=0){
        var f=new FLVER2();f.Header.Version=0x20014;f.Nodes=[new FLVER.Node{Name="Root",ParentIndex=-1,Scale=Vector3.One},new FLVER.Node{Name=child,ParentIndex=0,Translation=new(0,height,0),Rotation=new(0,0,angle),Scale=Vector3.One}];
        f.Materials=[new FLVER2.Material("#00#",@"N:\SPRJ\C[ARSN].mtd",0){Textures=new[]{"g_DiffuseTexture","g_SpecularTexture","g_ShininessTexture","g_BumpmapTexture"}.Select(name=>new FLVER2.Texture{ParamName=name,Path=@"N:\source.tif"}).ToList()}];
        f.BufferLayouts=[new FLVER2.BufferLayout{new(Type.Float3,Semantic.Position),new(Type.Float3,Semantic.Normal),new(Type.UByte4,Semantic.BoneIndices),new(Type.UByte4Norm,Semantic.BoneWeights),new(Type.Float4,Semantic.Tangent,0),new(Type.Float4,Semantic.Tangent,1),new(Type.Short4,Semantic.UV,0),new(Type.Float4,Semantic.VertexColor)}];
        var mesh=new FLVER2.Mesh{UseBoneWeights=true,MaterialIndex=0,VertexBuffers=[new(0)]};
        foreach(var p in new[]{new Vector3(0,1,0),new Vector3(1,1,0),new Vector3(0,1,1)}){
            var v=new FLVER.Vertex{Position=p,Normal=Vector3.UnitY,UVs=[new(0,0,0),new(.5f,.75f,0)],Tangents=[new(1,0,0,1),new(1,0,0,1)],Colors=[new(1f,1f,1f,1f)]};v.BoneIndices[0]=1;v.BoneWeights[0]=1;mesh.Vertices.Add(v);
        }
        mesh.FaceSets=[new(FLVER2.FaceSet.FSFlags.None,false,false,0,[0,1,2])];f.Meshes.Add(mesh);return f;
    }
    static List<TPF.Texture> Textures()=>[new("source",9,0,SkinRetarget.RgbaTexture([71,123,211,255]),TPF.TPFPlatform.PC)];
    static void CliChecks(byte[] source,byte[] native){
        var helper=Environment.GetEnvironmentVariable("SOULS_IMPORT_DLL");if(string.IsNullOrEmpty(helper))return;
        var root=Path.Combine(Path.GetTempPath(),"bb-skin-io-"+Guid.NewGuid());var game=Path.Combine(root,"source");var target=Path.Combine(root,"native");Directory.CreateDirectory(Path.Combine(game,"chr"));Directory.CreateDirectory(Path.Combine(target,"dvdroot_ps4/chr"));
        try{
            var src=new BND4();src.Files.Add(new BinderFile(Binder.FileFlags.Flag1,200,@"N:\c1430.flver",source));src.Files.Add(new BinderFile(Binder.FileFlags.Flag1,100,@"N:\source.tpf",new TPF{Textures=Textures()}.Write()));File.WriteAllBytes(Path.Combine(game,"chr/c1430.chrbnd"),src.Write());
            var dst=new BND4();dst.Files.Add(new BinderFile(Binder.FileFlags.Flag1,200,@"N:\c2630.flver",native));dst.Files.Add(new BinderFile(Binder.FileFlags.Flag1,300,@"N:\c2630.hkx",new byte[]{7,8,9}));var baseline=dst.Write();File.WriteAllBytes(Path.Combine(target,"dvdroot_ps4/chr/c2630.chrbnd.dcx"),baseline);
            var behavior=new byte[100];BitConverter.GetBytes(0x57e0e057u).CopyTo(behavior,0);System.Text.Encoding.ASCII.GetBytes(@"Behaviors\c2630.hkt").CopyTo(behavior,64);
            var animation=new BND4();animation.Files.Add(new BinderFile(Binder.FileFlags.Flag1,7000300,@"N:\c2630.hkx",behavior));animation.Files.Add(new BinderFile(Binder.FileFlags.Flag1,3000000,@"N:\c2630.tae",new TAE{Format=TAE.TAEFormat.DS3,ID=202630,Flags=new byte[8],Animations=[],SkeletonName="",SibName=""}.Write()));
            var animationBaseline=animation.Write();File.WriteAllBytes(Path.Combine(target,"dvdroot_ps4/chr/c2630.anibnd.dcx"),animationBaseline);Directory.CreateDirectory(Path.Combine(target,"dvdroot_ps4/action/script"));File.WriteAllBytes(Path.Combine(target,"dvdroot_ps4/action/script/c2630.hks"),[1,2,3,4]);
            (int code,string text) Run(string output,string? destination=null){
                var start=new System.Diagnostics.ProcessStartInfo(Environment.GetEnvironmentVariable("DOTNET")??"/usr/local/share/dotnet/dotnet"){RedirectStandardOutput=true,RedirectStandardError=true};
                foreach(var arg in new[]{helper,"bake-skin","--profile","DS3","--root",game,"--model","c1430","--native-root",target,"--template","c2630","--mask","0","--output",output})start.ArgumentList.Add(arg);
                if(destination!=null){start.ArgumentList.Add("--target");start.ArgumentList.Add(destination);}
                using var process=System.Diagnostics.Process.Start(start)!;var text=process.StandardOutput.ReadToEnd()+process.StandardError.ReadToEnd();Check(process.WaitForExit(30000),"Import CLI timed out");return(process.ExitCode,text);
            }
            var file=Path.Combine(root,"notes.txt");File.WriteAllText(file,"keep notes");Check(Run(file).code!=0 && File.ReadAllText(file)=="keep notes","Existing file overwritten");
            var folder=Path.Combine(root,"existing");Directory.CreateDirectory(folder);File.WriteAllText(Path.Combine(folder,"keep"),"keep folder");Check(Run(folder).code!=0 && File.ReadAllText(Path.Combine(folder,"keep"))=="keep folder","Existing folder overwritten");
            var insideSource=Path.Combine(game,"output");var insideNative=Path.Combine(target,"output");Check(Run(insideSource).code!=0 && !Directory.Exists(insideSource),"Source folder write accepted");Check(Run(insideNative).code!=0 && !Directory.Exists(insideNative),"Native game write accepted");
            var link=Path.Combine(root,"source-link");Directory.CreateSymbolicLink(link,game);Check(Run(Path.Combine(link,"output")).code!=0 && !Directory.Exists(insideSource),"Symlink into source accepted");
            var output=Path.Combine(root,"converted");var result=Run(output);Check(result.code==0,"Fixture CLI failed: "+result.text);var converted=BND4.Read(Path.Combine(output,"dvdroot_ps4/chr/c2630.chrbnd.dcx"));Check(converted.Files.Single(f=>f.ID==300).Bytes.Span.SequenceEqual(new byte[]{7,8,9}),"Native rig payload changed");
            var bank=converted.Files.Single(f=>f.ID==100).Bytes;Check(bank.Span.StartsWith("TPF\0"u8),"Embedded textures have an outer compression envelope");
            var tpf=TPF.Read(bank);Check(tpf.Platform==TPF.TPFPlatform.PS4 && tpf.Textures.All(t=>t.Name.StartsWith("c2630_import_")),"Invalid CLI texture bank");
            Check(File.ReadAllBytes(Path.Combine(target,"dvdroot_ps4/chr/c2630.chrbnd.dcx")).SequenceEqual(baseline),"Native source binder changed");Check(Run(output).code!=0,"Existing mod replaced");
            var independent=Path.Combine(root,"independent");var clone=Run(independent,"c6900");Check(clone.code==0,"Independent CLI failed: "+clone.text);var clonedAnimation=BND4.Read(Path.Combine(independent,"dvdroot_ps4/chr/c6900.anibnd.dcx"));
            Check(TAE.Read(clonedAnimation.Files.Single(f=>f.ID==3000000).Bytes).ID==206900,"Animation event-bank identity was not renamed");Check(clonedAnimation.Files.All(f=>!f.Name.Contains("c2630")),"Original binder names retained");
            Check(System.Text.Encoding.ASCII.GetString(clonedAnimation.Files.Single(f=>f.ID==7000300).Bytes.Span).Contains(@"Behaviors\c6900.hkt"),"Embedded behavior paths were not renamed");
            Check(File.ReadAllBytes(Path.Combine(independent,"dvdroot_ps4/action/script/c6900.hks")).SequenceEqual(new byte[]{1,2,3,4}),"Native action script changed");Check(File.ReadAllBytes(Path.Combine(target,"dvdroot_ps4/chr/c2630.anibnd.dcx")).SequenceEqual(animationBaseline),"Native animation source changed");
            Check(!File.Exists(Path.Combine(independent,"dvdroot_ps4/chr/c2630.chrbnd.dcx")),"Independent import replaces original model");
            File.WriteAllBytes(Path.Combine(target,"dvdroot_ps4/action/script/c6901.hks"),[9]);Check(Run(Path.Combine(root,"script-conflict"),"c6901").code!=0,"Existing native action script conflict accepted");
            Console.WriteLine("PASS: fixture binder conversion, preserved rig bytes, PS4 texture bank, exclusive export and source/symlink guards");
        }finally{Directory.Delete(root,true);}
    }
    static void Main(){
        System.Runtime.Loader.AssemblyLoadContext.Default.Resolving+=(ctx,name)=>{var dir=Environment.GetEnvironmentVariable("SMITHBOX_OUTPUT")!;var path=Path.Combine(dir,name.Name+".dll");return File.Exists(path)?ctx.LoadFromAssemblyPath(path):null;};
        var source=Fixture("L_Forearm",1);var native=Fixture("L Forearm",2,MathF.PI/2);var a=source.Write();var b=native.Write();var textures=Textures();
        var behavior=new byte[160];BitConverter.GetBytes(0x57e0e057u).CopyTo(behavior,0);System.Text.Encoding.ASCII.GetBytes(@"Behaviors\c2630.hkt").CopyTo(behavior,64);System.Text.Encoding.ASCII.GetBytes("c26300").CopyTo(behavior,96);System.Text.Encoding.ASCII.GetBytes("c2630").CopyTo(behavior,120);
        var renamed=SkinRetarget.RenameBehavior(behavior,"c2630","c6900");Check(System.Text.Encoding.ASCII.GetString(renamed).Contains(@"Behaviors\c6900.hkt") && System.Text.Encoding.ASCII.GetString(renamed).Contains("c26300") && renamed[120]==(byte)'c' && renamed[121]==(byte)'6',"Behavior path renaming failed");Check(behavior[121]==(byte)'2',"Behavior input changed");Reject(()=>SkinRetarget.RenameBehavior(behavior,"c2630","c69000"));
        var baked=SkinRetarget.Bake(a,b,textures,new HashSet<int>{0});Check(baked.Vertices==3 && baked.Triangles==1 && baked.DirectWeightShare==1,"Lost skin data");
        Near(baked.Model.Meshes[0].Vertices[0].Position,new(0,2,0));Near(baked.Model.Meshes[0].Vertices[1].Position,new(0,3,0));Near(baked.Model.Meshes[0].Vertices[1].Normal,new(-1,0,0));Check(baked.Model.Meshes[0].Vertices[0].BoneIndices[0]==1,"Global bone index changed");
        Check(baked.Model.Meshes[0].Vertices[0].UVs[1]==new Vector3(.5f,.75f,0),"UV data changed");Check(baked.Textures.Textures.All(t=>t.Name.StartsWith("c9998_import_")),"Texture ownership prefix missing");Check(baked.Model.Materials[0].Textures[0].Path.Contains(@"\chr\c9998\tex\c9998_import_"),"Texture ownership path missing");Check(baked.Textures.Platform==TPF.TPFPlatform.PS4 && baked.TextureFallbacks.Count==0,"Texture conversion missing");
        var reopened=TPF.Read(baked.Textures.Write());Check(reopened.Textures.Count==1 && reopened.Textures[0].Header.DXGIFormat==28,"Invalid serialized PS4 TPF");
        var dds=reopened.Textures[0].HeaderizeExt(out _);var images=Headerizer.GetDDSTextureBuffers(new DDS(dds),dds);Check(images[0].subImages[0].SequenceEqual(new byte[]{71,123,211,255}),"Texture payload changed");
        Check(a.SequenceEqual(source.Write()) && b.SequenceEqual(native.Write()),"Input FLVER changed");
        var compressed=new DDS{dwWidth=4,dwHeight=4,dwMipMapCount=1,dwPitchOrLinearSize=8};compressed.ddspf.dwFlags=DDS.DDPF.FOURCC;compressed.ddspf.dwFourCC="DXT1";
        var legacyTexture=new TPF.Texture("source",38,0,compressed.Write(new byte[8]),TPF.TPFPlatform.PC);var legacyBake=SkinRetarget.Bake(a,b,[legacyTexture]);Check(legacyBake.Textures.Textures[0].Format==0 && legacyBake.Textures.Textures[0].Header.DXGIFormat==71,"Legacy TPF format byte was not translated by pixel format");
        var scaledSource=Fixture("L_Forearm",1);var scaledTarget=Fixture("L Forearm",1);scaledTarget.Nodes[1].Scale=new(2,1,1);foreach(var v in scaledSource.Meshes[0].Vertices){v.Normal=Vector3.Normalize(new Vector3(1,1,0));v.Tangents=[new(1,-1,0,1),new(1,-1,0,1)];}
        var scaled=SkinRetarget.Bake(scaledSource.Write(),scaledTarget.Write(),textures).Model.Meshes[0].Vertices[1];Near(scaled.Position,new(2,1,0));Near(scaled.Normal,Vector3.Normalize(new Vector3(.5f,1,0)));Check(Math.Abs(Vector3.Dot(scaled.Normal,new(scaled.Tangents[0].X,scaled.Tangents[0].Y,scaled.Tangents[0].Z)))<.0001,"Nonuniform scale broke tangent orthogonality");
        source.Meshes[0].BoneIndices=[1];foreach(var v in source.Meshes[0].Vertices)v.BoneIndices[0]=0;
        Near(SkinRetarget.Bake(source.Write(),b,textures).Model.Meshes[0].Vertices[1].Position,new(0,3,0));
        // Falling back to a parent retains the unmatched child's bind offset.
        source=Fixture("LooseSleeve",1);foreach(var v in source.Meshes[0].Vertices){v.BoneIndices[0]=0;v.BoneWeights[0]=.75f;v.BoneIndices[1]=1;v.BoneWeights[1]=.25f;}
        var fallback=SkinRetarget.Bake(source.Write(),b,textures);Near(fallback.Model.Meshes[0].Vertices[0].Position,new(0,1,0));Check(Math.Abs(fallback.DirectWeightShare-.75)<.01,"Fallback confidence changed");
        var remap=new Dictionary<string,string>{{"LooseSleeve","L Forearm"}};Near(SkinRetarget.Bake(Fixture("LooseSleeve",1).Write(),b,textures,null,remap).Model.Meshes[0].Vertices[0].Position,new(0,2,0));
        source=Fixture("L_Forearm",1);source.Materials[0].Textures.RemoveAll(t=>t.ParamName=="g_ShininessTexture");var defaults=SkinRetarget.Bake(source.Write(),b,textures);Check(defaults.TextureFallbacks.Count==1 && defaults.Textures.Textures.Count==2,"Missing roughness fallback not reported");
        source.Materials[0].Textures.RemoveAll(t=>t.ParamName=="g_DiffuseTexture");Reject(()=>SkinRetarget.Bake(source.Write(),b,textures));
        source=Fixture("L_Forearm",1);foreach(var texture in source.Materials[0].Textures)texture.ParamName=texture.ParamName.Replace("Texture","");Check(SkinRetarget.Bake(source.Write(),b,textures).Textures.Textures.Count==1,"Legacy texture slots were not resolved");
        Reject(()=>SkinRetarget.Bake(a,b,textures,new HashSet<int>{1}));Reject(()=>SkinRetarget.Bake(a,b,[]));
        source=Fixture("L_Forearm",1);source.Nodes[0].ParentIndex=1;Reject(()=>SkinRetarget.Bake(source.Write(),b,textures));
        source=Fixture("L_Forearm",1);source.Meshes[0].Vertices[0].BoneIndices[0]=40;Reject(()=>SkinRetarget.Bake(source.Write(),b,textures));
        source=Fixture("L_Forearm",1);source.Meshes[0].Vertices[0].Position=new(float.NaN,0,0);Reject(()=>SkinRetarget.Bake(source.Write(),b,textures));
        source=Fixture("L_Forearm",1);source.Meshes[0].Vertices[0].BoneWeights[0]=0;Reject(()=>SkinRetarget.Bake(source.Write(),b,textures));
        source=Fixture("L_Forearm",1);source.Meshes[0].FaceSets[0].Indices[2]=99;Reject(()=>SkinRetarget.Bake(source.Write(),b,textures));
        source=Fixture("Unmatched",1);Reject(()=>SkinRetarget.Bake(source.Write(),b,textures));
        CliChecks(a,b);
        Console.WriteLine("PASS: global/palette bone retargeting, rotated bind poses, preserved ancestor offsets, overrides, render masks, PS4 texture payloads/fallbacks, unchanged inputs and malformed imports");
    }
}
