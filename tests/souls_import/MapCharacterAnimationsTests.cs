// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class MapCharacterAnimationsTests {
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Key(string p)=>new(p.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
    static string Hash(byte[] bytes)=>Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    static void Equal(Memory<byte> a,Memory<byte> b,string message)=>Check(a.Span.SequenceEqual(b.Span),message);
    static void Reject(Action a,string message){try{a();}catch(Exception e)when(e is InvalidDataException or NotSupportedException or ArgumentException or FileNotFoundException){return;}throw new Exception(message);}
    static Matrix4x4[] World(IReadOnlyList<Matrix4x4> local,IReadOnlyList<int> parents) {
        var result=new Matrix4x4[local.Count];var ready=new bool[result.Length];
        Matrix4x4 At(int i){if(ready[i])return result[i];ready[i]=true;return result[i]=local[i]*(parents[i]<0?Matrix4x4.Identity:At(parents[i]));}
        for(int i=0;i<result.Length;i++)At(i);return result;
    }
    static Matrix4x4[] ModelPose(FLVER2 model,IReadOnlyList<string> names,Matrix4x4[] pose) {
        var byName=names.Select((name,i)=>(name,i)).ToDictionary(p=>p.name,p=>p.i,StringComparer.Ordinal);var result=new Matrix4x4[model.Nodes.Count];var ready=new bool[result.Length];
        Matrix4x4 At(int i){if(ready[i])return result[i];ready[i]=true;return result[i]=byName.TryGetValue(model.Nodes[i].Name,out int bone)?pose[bone]:model.Nodes[i].ComputeLocalTransform()*(model.Nodes[i].ParentIndex<0?Matrix4x4.Identity:At(model.Nodes[i].ParentIndex));}
        for(int i=0;i<result.Length;i++)At(i);return result;
    }
    static Matrix4x4[] Inverses(FLVER2 model) {
        var world=World(model.Nodes.Select(n=>n.ComputeLocalTransform()).ToArray(),model.Nodes.Select(n=>(int)n.ParentIndex).ToArray());
        return world.Select(m=>{Check(Matrix4x4.Invert(m,out var inverse),"Singular skin reference");return inverse;}).ToArray();
    }
    static Vector3 Posed(FLVER2.Mesh mesh,FLVER.Vertex v,Matrix4x4[] inverse,Matrix4x4[] pose) {
        if(mesh.Dynamic==0)return Vector3.Transform(v.Position,pose[SkinRetarget.Bone(mesh,v.NormalW)]);
        Vector3 p=Vector3.Zero;float total=0;
        for(int i=0;i<4;i++){float weight=v.BoneWeights[i];if(weight==0)continue;int bone=SkinRetarget.Bone(mesh,v.BoneIndices[i]);p+=Vector3.Transform(v.Position,inverse[bone]*pose[bone])*weight;total+=weight;}
        Check(total>.99f,"Source mesh lacks normalized skin weights");return p/total;
    }
    public static void Main(string[] args) {
        if(args.Length!=3)throw new ArgumentException("Supply DS1R, native dvdroot_ps4 and DS1R parameter definitions");
        string sourceRoot=args[0],nativeRoot=args[1],defs=args[2];var hashes=new Dictionary<string,string>();
        byte[]? Read(string p){var path=Path.Combine(sourceRoot,p);if(!File.Exists(path))return null;var b=File.ReadAllBytes(path);hashes.TryAdd(path,Hash(b));return b;}
        byte[] Native(string p){var path=Path.Combine(nativeRoot,p);var b=File.ReadAllBytes(path);hashes.TryAdd(path,Hash(b));return b;}
        var msb=MSB1.Read(Read("map/MapStudio/m10_02_00_00.msb")!);var selections=new[]{msb.Parts.Enemies.Single(p=>p.EntityID==6270),msb.Parts.Enemies.Single(p=>p.EntityID==6080),msb.Parts.Enemies.Single(p=>p.EntityID==6060)};
        byte[] nativeBank=Native("chr/c8030.anibnd.dcx"),nativeScript=Native("action/script/c8030.hks");
        var chr=BND4.Read(Native("chr/c8030.chrbnd.dcx"));byte[] nativeModel=chr.Files.Single(f=>Leaf(f.Name)=="c8030.flver").Bytes.ToArray();
        var skeleton=BND4.Read(nativeBank).Files.Single(f=>f.ID==1000000).Bytes.ToArray();var rig=NpcAnimationInspector.Skeleton(skeleton);
        Check(rig.Names.Length==129&&rig.Name=="Master","Native Doll rig changed");var results=new List<object>();
        foreach(var source in selections) {
            FLVER2 original=source.ModelName=="c0000"?NpcAppearance.Load(Read,defs,source.CharaInitID,applyBodyShape:true).Model:
                FLVER2.Read(BND3.Read(Read("chr/c2750.chrbnd.dcx")!).Files.Single(f=>Leaf(f.Name)=="c2750.flver").Bytes);
            var textures=original.Materials.SelectMany(m=>m.Textures).Select(t=>Leaf(t.Path).Split('.')[0]).Distinct(StringComparer.OrdinalIgnoreCase)
                .Select(n=>new TPF.Texture(n,9,0,SkinRetarget.RgbaTexture([255,255,255,255]),TPF.TPFPlatform.PC)).ToArray();
            var skin=SkinRetarget.Bake(original.Write(),nativeModel,textures,sourceProfile:"DS1R",preserveSourceDimensions:true);skin=skin with{ProportionScale=SkinRetarget.HumanoidScale(original.Nodes,skin.Model.Nodes)};
            string target="c"+(9800+results.Count);var input=BND4.Read(nativeBank);
            foreach(var f in input.Files){f.Name=f.Name.Replace("c8030",target);if(f.ID is >=7000000 and <8000000)f.Bytes=SkinRetarget.RenameBehavior(f.Bytes,"c8030",target);}
            // Unrelated prior-stage idle audio must survive the bounded Doll filter.
            var taeFile=input.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));var fixtureTae=TAE.Read(taeFile.Bytes);var idle=fixtureTae.Animations.Single(a=>a.ID==0);
            byte[] audio=new byte[16];BitConverter.GetBytes(1).CopyTo(audio,0);BitConverter.GetBytes(123456789).CopyTo(audio,4);
            idle.Events.Add(new TAE.Event(.1f,.2f,128,idle.Events[0].Unk04,audio,false));
            var mask=idle.Events.First(e=>e.Type==315).GetClone(false);var maskBytes=mask.GetParameterBytes(false).ToArray();BitConverter.GetBytes(7).CopyTo(maskBytes,16);mask.SetParameterBytes(false,maskBytes);idle.Events.Add(mask);taeFile.Bytes=fixtureTae.Write();
            var before=input.Write(input.Compression);string path=$"chr/{target}.anibnd.dcx";var files=new Dictionary<string,byte[]>{{path,before},{"action/script/"+target+".hks",nativeScript},{"unrelated.bin",[1,2,3]}};
            var report=MapCharacterAnimations.Apply(Read,source,target,skin,files);var after=BND4.Read(files[path]);var beforeRead=BND4.Read(before);
            int expectedID=source.EntityID==6270?7810:0,frames=source.EntityID==6270?91:source.EntityID==6080?61:121;
            Check(report.SourceClipID==expectedID&&report.FrameCount==frames&&report.Approximation==(source.EntityID==6080),"NPC source pose or approximation report is wrong");
            Check(report.SourceArchive==(source.EntityID==6270?"chr/c0000_a00_md.anibnd.dcx":source.EntityID==6080?"chr/c0000_a00_lo.anibnd.dcx":"chr/c2750.anibnd.dcx"),"Split source provenance is wrong");
            Check(files.Count==3&&files["action/script/"+target+".hks"].SequenceEqual(nativeScript)&&files["unrelated.bin"].SequenceEqual(new byte[]{1,2,3}),"Pose staging changed HKS or unrelated output");
            var checkedTae=TAE.Read(after.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);var beforeTae=TAE.Read(beforeRead.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);
            foreach(int slot in report.NativeIdleSlots) {
                var row=checkedTae.Animations.Single(a=>a.ID==slot);var old=beforeTae.Animations.Single(a=>a.ID==slot);
                Check(row.AnimFileName==old.AnimFileName&&row.Events.All(e=>e.StartTime>=0&&e.EndTime<=report.Duration+.00001f),"Idle row metadata or timing is invalid");
                Check(!row.Events.Any(e=>e.Type==313||e.Type is 66 or 67),"Copied Doll idle effect/additive modifier survived");
                Check(!row.Events.Any(e=>e.Type==128&&BitConverter.ToInt32(e.GetParameterBytes(false),4)==803006500),"Doll idle voice survived");
                if(slot==0)Check(row.Events.Any(e=>e.Type==128&&e.GetParameterBytes(false).SequenceEqual(audio))&&row.Events.Any(e=>e.Type==315&&BitConverter.ToInt32(e.GetParameterBytes(false),16)==7),"Unrelated prior-stage idle event was removed");
                checkedTae.Animations[checkedTae.Animations.IndexOf(row)]=old;
            }
            Equal(checkedTae.Write(),beforeTae.Write(),"A non-idle TAE row/group/header changed");
            foreach(var f in after.Files) {
                var old=beforeRead.Files.Single(p=>p.ID==f.ID);Check(f.Name==old.Name&&f.Flags==old.Flags&&f.CompressionType==old.CompressionType,"Animation binder entry metadata changed");
                if(!report.NativeIdleSlots.Contains(f.ID-1000000000)&&!TAE.Is(f.Bytes.ToArray()))Equal(f.Bytes,old.Bytes,"A non-idle clip, skeleton or behavior changed");
            }
            byte[] output=after.Files.Single(f=>f.ID==1000000000).Bytes.ToArray();foreach(int slot in report.NativeIdleSlots)Equal(output,after.Files.Single(f=>f.ID==1000000000+slot).Bytes,"A Doll idle variation was not replaced");
            var inspector=new NpcAnimationInspector(output);var decoded=inspector.Animation(frames,rig.Names.Length);
            Check(inspector.Binding(rig.Name,decoded.Object,rig.Names.Length)&&inspector.TrackNames(decoded.Object).SequenceEqual(rig.Names),"NPC clip binding or annotation order differs from native rig");
            Check(Math.Abs(decoded.Duration-report.Duration)<.00001f,"NPC loop duration changed");
            var clip=MapCharacterAnimations.Load(Read,source.ModelName,expectedID).Animation;
            Reject(()=>NativeAnimation.Convert(clip,skeleton,[],skin.ProportionScale,preserveSourceWorld:true),"NPC pose accepted an appearance without skin mappings");
            var sourceIndices=clip.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(p=>p.Name,p=>p.i,StringComparer.Ordinal);
            var targetIndices=rig.Names.Select((n,i)=>(n,i)).ToDictionary(p=>p.n,p=>p.i,StringComparer.Ordinal);
            var sourceInverse=Inverses(original);var targetInverse=Inverses(skin.Model);var meshProof=new List<object>();
            foreach(int frame in new[]{0,frames/2,frames-1}) {
                var sw=World(clip.Sample(frame==frames-1?clip.Duration:frame*clip.FrameDuration),clip.Bones.Select(b=>b.Parent).ToArray());var tw=World(inspector.Pose(decoded,frame),rig.Parents);
                foreach(string landmark in new[]{"head","lhand","rhand","lfoot","rfoot"}) {
                    int s=sourceIndices.Single(p=>Key(p.Key)==landmark).Value,t=targetIndices.Single(p=>Key(p.Key)==landmark).Value;
                    Check(Vector3.Distance(sw[s].Translation,tw[t].Translation*skin.ProportionScale)<.005f,$"{source.EntityID}: {landmark} lost its source world pose");
                }
                var sp=ModelPose(original,clip.Bones.Select(b=>b.Name).ToArray(),sw);var tp=ModelPose(skin.Model,rig.Names,tw);var errors=new List<float>();
                for(int m=0;m<original.Meshes.Count;m++)for(int v=0;v<original.Meshes[m].Vertices.Count;v++)errors.Add(Vector3.Distance(Posed(original.Meshes[m],original.Meshes[m].Vertices[v],sourceInverse,sp),Posed(skin.Model.Meshes[m],skin.Model.Meshes[m].Vertices[v],targetInverse,tp)*skin.ProportionScale));
                int rigidHeadEdges=0,hairEdges=0;float rigidHeadMaxError=0;
                bool HeadVertex(FLVER2.Mesh mesh,FLVER.Vertex v)=>Enumerable.Range(0,4).Any(j=>v.BoneWeights[j]>.9999f&&Key(original.Nodes[SkinRetarget.Bone(mesh,v.BoneIndices[j])].Name)=="head");
                for(int m=0;m<original.Meshes.Count;m++) {
                    var sm=original.Meshes[m];var tm=skin.Model.Meshes[m];bool hair=original.Materials[sm.MaterialIndex].Textures.Any(t=>Leaf(t.Path).Contains("HR_",StringComparison.OrdinalIgnoreCase));
                    var positions=sm.Vertices.Select(v=>Posed(sm,v,sourceInverse,sp)).ToArray();var emitted=tm.Vertices.Select(v=>Posed(tm,v,targetInverse,tp)*skin.ProportionScale).ToArray();
                    for(int v=0;v<sm.Vertices.Count;v++)if(HeadVertex(sm,sm.Vertices[v]))rigidHeadMaxError=Math.Max(rigidHeadMaxError,Vector3.Distance(positions[v],emitted[v]));
                    foreach(var face in sm.FaceSets.Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None)) {
                        var indices=face.Triangulate(sm.Vertices.Count<65535);
                        for(int t=0;t<indices.Count;t+=3)for(int e=0;e<3;e++) {
                            int a=indices[t+e],b=indices[t+(e+1)%3];if(!HeadVertex(sm,sm.Vertices[a])||!HeadVertex(sm,sm.Vertices[b]))continue;
                            float length=Vector3.Distance(positions[a],positions[b]);if(length<.005f)continue;
                            Check(Math.Abs(Vector3.Distance(emitted[a],emitted[b])-length)<.00005f,"Source rigid head/hair edge dimensions changed after native playback and actor scale");
                            ++rigidHeadEdges;if(hair)++hairEdges;
                        }
                    }
                }
                if(source.ModelName=="c0000")Check(rigidHeadEdges>1000&&hairEdges>100&&rigidHeadMaxError<.001f,"Actual source head/hair pose or dimensions were not preserved");
                float mean=errors.Average(),max=errors.Max(),p95=errors.Order().ElementAt((int)(errors.Count*.95));
                // Multi-bone armor retains blended bind-remap residuals even
                // when rigid dimensions are preserved. The separate head/hair
                // and joint checks above apply tighter anatomical bounds.
                Check(mean<.04f&&p95<.08f&&max<.15f,$"{source.EntityID}: actual posed mesh exceeds source-bind remap tolerances (mean{mean},p95{p95},max{max})");
                meshProof.Add(new{frame,mean,p95,max,vertices=errors.Count,rigidHeadEdges,hairEdges,rigidHeadMaxError});
            }
            var bad=BND4.Read(before);bad.Files.RemoveAll(f=>f.ID==1002000000);var badFiles=new Dictionary<string,byte[]>{{path,bad.Write(bad.Compression)}};var untouched=badFiles[path];
            Reject(()=>MapCharacterAnimations.Apply(Read,source,target,skin,badFiles),"Missing native idle slot accepted");Equal(badFiles[path],untouched,"Failed NPC staging partially mutated its output");
            results.Add(new{source.EntityID,report,meshProof});
            Console.WriteLine($"PASS NPC{source.EntityID}: {expectedID}, {frames}frames, all3 idle variants, source hands/feet and actual posed geometry");
        }
        // Exact-name resolution must not silently choose a missing or duplicate split clip.
        Reject(()=>MapCharacterAnimations.Load(p=>p=="chr/c0000_a00_md.anibnd.dcx"?null:Read(p),"c0000",7810),"Missing split idle was silently substituted");
        var main=BND3.Read(Read("chr/c0000.anibnd.dcx")!);var middle=BND3.Read(Read("chr/c0000_a00_md.anibnd.dcx")!);var duplicate=middle.Files.Single(f=>Leaf(f.Name)=="a00_7810.hkx").Clone();main.Files.Add(duplicate);var ambiguous=main.Write(main.Compression);
        Reject(()=>MapCharacterAnimations.Load(p=>p=="chr/c0000.anibnd.dcx"?ambiguous:Read(p),"c0000",7810),"Duplicate main/split animation was silently selected");
        Reject(()=>MapCharacterAnimations.Load(Read,"../c0000",7810),"Source animation path traversal accepted");
        CombatCompatibility(Read,Native);
        foreach(var (path,hash) in hashes)Check(Hash(File.ReadAllBytes(path))==hash,"Original game asset changed: "+path);
        var reportPath=Path.Combine(Directory.GetCurrentDirectory(),"out/map-editor/map-character-animation-tests/results.json");Directory.CreateDirectory(Path.GetDirectoryName(reportPath)!);File.WriteAllText(reportPath,JsonSerializer.Serialize(new{results,originalFiles=hashes.Count,gameplayValidated=false},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS NPC source idle conversion; {hashes.Count} original source/native assets unchanged. Gameplay remains unverified.");
    }

    static void CombatCompatibility(Func<string,byte[]?> read,Func<string,byte[]> native) {
        // Captured from the compiled four-argument converter at2ed432e BEFORE
        // adding NPC mode, using the actual source geometry mappings and native
        // c2610 rig. These are byte checks, not a second call to the new branch.
        var expected=new Dictionary<(string,int),string> {
            [("c2900",9001)]="8f8e0b76586181bf4bc9b9b9ddb2e978cc968ae2bf2504c2b32e8d245789d9de",
            [("c2900",9061)]="ab8bdcbce0b9b8e64129b35735f3d8e329eae6189308be487717e06503e87123",
            [("c2900",2014)]="f015a57d676728ae54f4a936fe25b9f28aee76ef0f23c3878e35f73ec25f942e",
            [("c2900",2020)]="3de4b5c4ae293d2bfa02c9e1d2697c34c70a40446137129453500fd42e753147",
            [("c2900",2040)]="080fbcfd95aed52a59593139bbec09a6daac471fb34cdc6b87d0b185f0b4cdf7",
            [("c2910",9001)]="328a0f24a0ffdaeea7ea9cd72642b23d87b84143260c53792c03b6f75b0a2acc",
            [("c2910",9061)]="cd20f8c74b6ea2851fc4aea1a9de3485656ccb5c1be387a22c0512201f7ca4dc",
            [("c2910",2014)]="cb603a7082f8a41b305099223f7eaa74a41af7b4fb0c5638986eda86dff7909f",
            [("c2910",2020)]="548d4b3710f60ddc7c07c62baf8f35b1f4c5ddefcaa335b980dc479d1c612f0b",
            [("c2910",2040)]="7409f54344f046c5d6119f4f41dde4a5d7b17f36ba60320fd5f3d08fa2f9a050",
        };
        var skeleton=BND4.Read(native("chr/c2610.anibnd.dcx")).Files.Single(f=>f.ID==1000000).Bytes.ToArray();
        var target=BND4.Read(native("chr/c2610.chrbnd.dcx")).Files.Single(f=>Leaf(f.Name)=="c2610.flver").Bytes.ToArray();
        foreach(string model in new[]{"c2900","c2910"}) {
            var source=BND3.Read(read("chr/"+model+".chrbnd.dcx")!).Files.Single(f=>Leaf(f.Name)==model+".flver").Bytes.ToArray();var flver=FLVER2.Read(source);
            var textures=flver.Materials.SelectMany(m=>m.Textures).Select(t=>Leaf(t.Path).Split('.')[0]).Distinct(StringComparer.OrdinalIgnoreCase).Select(n=>new TPF.Texture(n,9,0,SkinRetarget.RgbaTexture([255,255,255,255]),TPF.TPFPlatform.PC)).ToArray();
            var skin=SkinRetarget.Bake(source,target,textures,sourceProfile:"DS1R",fitSegments:true,allowLargeHumanoid:true);float scale=SkinRetarget.HumanoidScale(flver.Nodes,skin.Model.Nodes,true);
            foreach(int id in new[]{9001,9061,2014,2020,2040}) {
                var clip=SourceAnimation.Load(read,model,id);var bytes=NativeAnimation.Convert(clip,skeleton,skin.Bones,scale);
                Check(Hash(bytes)==expected[(model,id)],"Default native-rest combat animation bytes changed: "+model+"/"+id);
            }
        }
        Console.WriteLine("PASS all10 actual source skeleton combat/wake clips retain pre-NPC-mode byte hashes");
    }
}
