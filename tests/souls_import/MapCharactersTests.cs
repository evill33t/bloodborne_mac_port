// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class MapCharactersTests {
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Equal(Memory<byte> actual,Memory<byte> expected,string message)=>Check(actual.Span.SequenceEqual(expected.Span),message);
    static void Reject(Action action,string message) {try{action();}catch(InvalidDataException){return;}catch(FileNotFoundException){return;}catch(NotSupportedException){return;}throw new Exception(message);}
    static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z);
    sealed class NativeFixture : IDisposable {
        internal string Root {get;}=Path.Combine(Path.GetTempPath(),"bbmap-character-allocation-"+Guid.NewGuid().ToString("N"));
        internal NativeFixture(Func<string,byte[]> read) {
            foreach(string path in new[]{"map/MapStudio/m21_00_00_00.msb.dcx","chr/c8030.chrbnd.dcx","chr/c8030.anibnd.dcx","action/script/c8030.hks"}) {
                string target=Path.Combine(Root,path);Directory.CreateDirectory(Path.GetDirectoryName(target)!);File.WriteAllBytes(target,read(path));
            }
            Directory.CreateDirectory(Path.Combine(Root,"sound"));
            // Occupancy must cover a project, a localized wavebank and a limiter
            // even when the corresponding chr/HKS files do not exist.
            foreach(string name in new[]{"sprj_c8901.fev","sprj_c8902_eng.fsb","sprj_c8903.itl"})File.WriteAllBytes(Path.Combine(Root,"sound",name),[]);
        }
        public void Dispose()=>Directory.Delete(Root,true);
    }
    public static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],defs=args[2];var hashes=new Dictionary<string,byte[]>();
        byte[]? Read(string path){string full=Path.Combine(sourceRoot,path);if(!File.Exists(full))return null;byte[] bytes=File.ReadAllBytes(full);hashes.TryAdd(full,SHA256.HashData(bytes));return bytes;}
        byte[] Native(string path){string full=Path.Combine(nativeRoot,path);byte[] bytes=File.ReadAllBytes(full);hashes.TryAdd(full,SHA256.HashData(bytes));return bytes;}
        const string mapName="m10_02_00_00";var source=MSB1.Read(Read($"map/MapStudio/{mapName}.msb")!);
        var mapBytes=Native("map/MapStudio/m24_01_00_00.msb.dcx");var destination=MSBB.Read(mapBytes);string collision=destination.Parts.Collisions.First().Name;var offset=new Vector3(-143.8f,20.07f,164.63f);
        // A preceding mod stage's opaque parameter change must survive this one.
        var oldGame=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx"));
        var unrelated=oldGame.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param");byte[] stagedObject=unrelated.Bytes.ToArray();stagedObject[^1]^=1;unrelated.Bytes=stagedObject;
        byte[] stagedGame=oldGame.Write(DCX.Type.DCX_DFLT_10000_44_9);
        // The allocator must respect MSB models, all entity namespaces and files.
        destination.Models.Enemies.Add(new MSBB.Model.Enemy("c8900"));
        destination.Regions.GetEntries()[0].EntityID=2419700;destination.Events.GetEntries()[0].EntityID=2419701;
        byte[] beforeDestination=destination.Write();
        using var nativeFixture=new NativeFixture(Native);
        var result=MapCharacters.BakeFirelink(Read,"DS1R",mapName,defs,nativeFixture.Root,destination,offset,collision,stagedGame);
        Equal(destination.Write(),beforeDestination,"Character staging mutated the destination MSB");
        Check(result.Actors.Count==3&&result.Models.Count==3&&result.Files.Count==10,"Expected three independent native character packages plus composed game parameters");
        Check(result.Reports.Count==source.Parts.Enemies.Count&&result.Reports.Select(r=>r.SourceName).Distinct().Count()==result.Reports.Count,"Not all source characters were accounted for exactly once");
        var active=result.Reports.Where(r=>r.Status=="placed-source-idle-ai-disabled").ToArray();Check(active.Length==3,"Unsupported source NPCs were instantiated");
        Check(active.All(r=>r.SourcePose!=null)&&active.Single(r=>r.CharaInitID==6270).SourcePose!.SourceClipID==7810&&
            active.Single(r=>r.SourceModel=="c2750").SourcePose!.SourceClipID==0,"Source initial NPC poses were not selected");
        Check(active.Single(r=>r.CharaInitID==6080).SourcePose!.Approximation,"Petrus equipment-driven idle limitation was hidden");
        Check(active.Where(r=>r.SourceModel=="c0000").Select(r=>r.CharaInitID).Order().SequenceEqual(new[]{6080,6270}),"Wrong human NPC appearances selected");
        Check(active.Any(r=>r.SourceModel=="c2750"&&r.CharaInitID==-1),"Authored Anastacia source model missing");
        Check(active.All(r=>r.EntityID>2419701&&r.EntityID<=2419799&&r.TargetModel!="c8900"),"Model/entity allocator collided with destination data");
        Check(active.All(r=>int.Parse(r.TargetModel![1..]) is >=8904 and <9000),"Character allocator used a reserved sound identity or a model outside the native sound-bank range");
        Check(result.Models.All(m=>int.Parse(m.Name[1..])<9000)&&result.Actors.All(a=>int.Parse(a.ModelName[1..])<9000),"Imported actor model cannot register a native character sound bank");
        Check(result.Reports.Where(r=>r.TargetModel==null).All(r=>r.Vertices==0&&r.Limitations.Any(l=>l.Contains("not instantiated"))),"Disabled quest NPCs look like converted actors");
        Check(result.Reports.Where(r=>r.SourceModel=="c0000"&&r.TargetModel==null).All(r=>r.Status=="disabled-source-quest-or-interaction"),"Future quest arrivals were spawned unconditionally");
        var systemTextures=TPF.Read(Native("other/systex.tpf.dcx")).Textures.Select(t=>Leaf(t.Name).Split('.')[0]).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var nativeCharacter=BND4.Read(Native("chr/c8030.chrbnd.dcx"));var nativeAnimation=BND4.Read(Native("chr/c8030.anibnd.dcx"));var nativeScript=Native("action/script/c8030.hks");
        var nativeVisual=FLVER2.Read(nativeCharacter.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);
        foreach(var report in active) {
            string target=report.TargetModel!;int id=int.Parse(target[1..]);var actor=result.Actors.Single(a=>a.ModelName==target);var original=source.Parts.Enemies.Single(p=>p.Name==report.SourceName);
            Check(actor.EntityID==report.EntityID&&actor.NPCParamID==id*100&&actor.ThinkParamID==-1,"Actor is not bound to its unique native parameters");
            Check(actor.TalkID==-1&&actor.CharaInitID==-1&&actor.InitAnimID==-1&&actor.DamageAnimID==-1&&actor.PatrolInfoName==null&&actor.MovePointNames.All(p=>p==null),"Actor retained source/native quest, initial animation or patrol references");
            Check(actor.CollisionName==collision&&actor.Position==original.Position+offset&&actor.Rotation==original.Rotation&&Vector3.Distance(actor.Scale,original.Scale*report.ProportionScale)<.0001f,"Source placement transform was not retained");
            Check(report.SourcePosition==original.Position&&report.SourceRotation==original.Rotation&&report.SourceScale==original.Scale&&report.SourceEntityID==original.EntityID&&report.SourceTalkID==original.TalkID,"Report lost source placement/quest metadata");
            Check(actor.DrawGroups.Concat(actor.DispGroups).Concat(actor.BackreadGroups).All(v=>v==uint.MaxValue),"Source/native actor masks hide imported geometry");
            var character=BND4.Read(result.Files[$"chr/{target}.chrbnd.dcx"]);var animation=BND4.Read(result.Files[$"chr/{target}.anibnd.dcx"]);
            Check(character.Files.All(f=>!f.Name.Contains("c8030",StringComparison.OrdinalIgnoreCase))&&animation.Files.All(f=>!f.Name.Contains("c8030",StringComparison.OrdinalIgnoreCase)),"Independent package retained template paths");
            Check(character.Files.All(f=>!Leaf(f.Name).EndsWith("_c.hkx")&&!Leaf(f.Name).EndsWith(".clm2")&&Leaf(f.Name)!="div_data.txt"),"Unsafe foreign-vertex cloth bindings remain");
            foreach(var f in nativeCharacter.Files.Where(f=>Leaf(f.Name).EndsWith(".hkx",StringComparison.OrdinalIgnoreCase)&&!Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)))Equal(character.Files.Single(n=>n.ID==f.ID).Bytes,f.Bytes,"Native collision/skeleton packfile changed");
            Equal(result.Files[$"action/script/{target}.hks"],nativeScript,"Native idle HKS changed");
            foreach(var f in animation.Files) {
                var originalFile=nativeAnimation.Files.Single(n=>n.ID==f.ID);
                if(TAE.Is(f.Bytes.ToArray())) {
                    var tae=TAE.Read(f.Bytes);var originalTae=TAE.Read(originalFile.Bytes);Check(tae.ID==200000+id&&tae.Animations.Count==originalTae.Animations.Count,"Renamed TAE lost animation/event data");
                    foreach(int slot in report.SourcePose!.NativeIdleSlots) {
                        var row=tae.Animations.Single(a=>a.ID==slot);
                        Check(row.Events.All(e=>e.StartTime>=0&&e.EndTime<=report.SourcePose.Duration+.00001f),"Imported idle TAE timing exceeds its source loop");
                        tae.Animations[tae.Animations.IndexOf(row)]=originalTae.Animations.Single(a=>a.ID==slot);
                    }
                    tae.ID=originalTae.ID;Equal(tae.Write(),originalTae.Write(),"NPC pose conversion changed a non-idle TAE row or metadata");
                }
                else if(f.ID>=7000000&&f.ID<8000000) {Equal(f.Bytes,SkinRetarget.RenameBehavior(originalFile.Bytes,"c8030",target),"Behavior rename changed fixup-safe clone semantics");Check(!System.Text.Encoding.ASCII.GetString(f.Bytes.Span).Contains("c8030",StringComparison.OrdinalIgnoreCase),"Behavior still addresses the original Doll character");}
                else if(report.SourcePose!.NativeIdleSlots.Contains(f.ID-1000000000)) {
                    Check(!f.Bytes.Span.SequenceEqual(originalFile.Bytes.Span),"Imported idle retained the native Doll pose");
                    var pack=new NativeAnimation.Pack(f.Bytes.ToArray());Check(Math.Abs(pack.Float(pack.Object("hkaSplineCompressedAnimation")+16)-report.SourcePose.Duration)<.00001f,"NPC pose playback duration differs from source");
                }
                else Equal(f.Bytes,originalFile.Bytes,"Native animation/skeleton changed");
            }
            var flver=FLVER2.Read(character.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);var textureEntry=character.Files.Single(f=>TPF.Is(f.Bytes.ToArray()));
            Check(!DCX.Is(textureEntry.Bytes.ToArray()),"Character's inner TPF has an invalid DCX wrapper");var tpf=TPF.Read(textureEntry.Bytes);Check(tpf.Platform==TPF.TPFPlatform.PS4&&tpf.Textures.Count>0,"Character lacks converted PS4 textures");
            var names=tpf.Textures.Select(t=>t.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
            foreach(var diffuse in flver.Materials.SelectMany(m=>m.Textures).Where(t=>t.ParamName=="g_DiffuseTexture")) {
                var color=tpf.Textures.Single(t=>t.Name==Leaf(diffuse.Path).Split('.')[0]);Check(new[]{29,91,72,75,78,99}.Contains(color.Header.DXGIFormat),"NPC diffuse texture is not tagged sRGB");
            }
            Check(report.ProportionScale is >.7f and <1&&actor.Scale.X<original.Scale.X,"Native rig still inflates source NPC stature");
            Check(flver.Materials.SelectMany(m=>m.Textures).Where(t=>t.ParamName is "g_DiffuseTexture" or "g_SpecularTexture" or "g_ShininessTexture" or "g_BumpmapTexture").All(t=>names.Contains(Leaf(t.Path).Split('.')[0])||t.Path.StartsWith(@"N:\SPRJ\data\Other\SysTex\",StringComparison.OrdinalIgnoreCase)&&systemTextures.Contains(Leaf(t.Path).Split('.')[0])),"Converted character has missing material texture bindings");
            Check(flver.Nodes.Select(n=>n.Name).SequenceEqual(nativeVisual.Nodes.Select(n=>n.Name)),"Imported model is incompatible with its native skeleton");
            Check(report.Vertices==flver.Meshes.Sum(m=>m.Vertices.Count)&&report.Vertices>10000&&report.Triangles>10000&&report.DirectWeightShare>.99,"Real source geometry was lost or replaced by a placeholder");
            foreach(var mesh in flver.Meshes)foreach(var v in mesh.Vertices) {
                Check(mesh.UseBoneWeights&&Finite(v.Position)&&Finite(v.Normal)&&Math.Abs(v.Normal.Length()-1)<.02f,"Invalid native skinned vertex");float total=0;
                for(int n=0;n<4;++n){float weight=v.BoneWeights[n];if(weight==0)continue;int bone=SkinRetarget.Bone(mesh,v.BoneIndices[n]);Check(bone>=0&&bone<flver.Nodes.Count&&float.IsFinite(weight)&&weight>=0,"Invalid native bone weight reference");total+=weight;}Check(Math.Abs(total-1)<2.01f/255,"Native weights exceed four-channel 8-bit quantization error");
            }
            if(report.SourceModel=="c0000") {
                Check(report.SourceParts.Contains("facegen/FaceGen.fgbnd")&&report.SourceParts.Any(p=>p.Contains("/HR_")),"Human actor lacks actual source face/hair assets");
                Check(names.Any(n=>n.Contains("FG_import_"+report.CharaInitID)),"Human actor lacks its distinct EGT face diffuse");
                var assembled=NpcAppearance.Load(Read,defs,report.CharaInitID,applyBodyShape:true);Check(report.Vertices==assembled.Vertices&&report.Triangles==assembled.Triangles,"Source material render masks removed human geometry (including groups above 31)");
                for(int m=0;m<assembled.Model.Meshes.Count;m++)for(int v=0;v<assembled.Model.Meshes[m].Vertices.Count;v++)for(int uv=0;uv<2;uv++) {
                    var delta=Vector3.Abs(flver.Meshes[m].Vertices[v].UVs[uv]-assembled.Model.Meshes[m].Vertices[v].UVs[uv]);
                    Check(delta.X<=.5001f/1024+.5001f/2048&&delta.Y<=.5001f/1024+.5001f/2048,"Packaged human UV set scaled/wrapped instead of preserving source coordinates");
                }
                // Independently compare the authored source hair's edge lengths
                // with the actual packaged FLVER under its decoded idle pose.
                // This bypasses the appearance composer's vertex calculations.
                string hairPath=assembled.Parts.Single(p=>p.Slot=="Hair").Path;
                Check(assembled.BodyShape!=null,"Packaged human omitted source body-shape provenance");
                float sourceHeadFactor=assembled.BodyShape!.Bones.Single(b=>b.Bone=="Head").Scale.X;
                Check(Math.Abs(sourceHeadFactor-(report.CharaInitID==6080?1.10f:1.04f))<.00001f,"Source NPC Head control/BSI limit was not applied");
                var rawHair=FLVER2.Read(BND3.Read(Read(hairPath)!).Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);
                var hairMeshes=assembled.Model.Meshes.Select((m,i)=>(m,i)).Where(p=>assembled.Model.Materials[p.m.MaterialIndex].Textures.Any(t=>Leaf(t.Path).Contains("HR_",StringComparison.OrdinalIgnoreCase))).ToArray();
                Check(hairMeshes.Length==rawHair.Meshes.Count,"Packaged appearance lost an authored hair subset");
                var rig=NpcAnimationInspector.Skeleton(animation.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
                var inspector=new NpcAnimationInspector(animation.Files.Single(f=>f.ID==1000000000).Bytes.ToArray());var idle=inspector.Animation(report.SourcePose!.FrameCount,rig.Names.Length);
                var pose=NativeAnimation.Worlds(inspector.Pose(idle,idle.Poses.Length/2),rig.Parents);int head=flver.Nodes.FindIndex(n=>n.Name=="Head"),poseHead=Array.IndexOf(rig.Names,"Head");
                Check(head>=0&&poseHead>=0,"Missing packaged head bind");
                Check(Matrix4x4.Invert(SkinRetarget.World(flver.Nodes)[head],out var inverseHead),"Singular packaged head bind");int edges=0;
                for(int m=0;m<rawHair.Meshes.Count;m++) {
                    var raw=rawHair.Meshes[m];var emitted=flver.Meshes[hairMeshes[m].i];Check(raw.Vertices.Count==emitted.Vertices.Count,"Packaged hair vertex correspondence changed");
                    foreach(var face in raw.FaceSets.Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None)) {
                        var indices=face.Triangulate(raw.Vertices.Count<65535);
                        for(int t=0;t<indices.Count;t+=3)for(int e=0;e<3;e++) {
                            int a=indices[t+e],b=indices[t+(e+1)%3];float expected=Vector3.Distance(raw.Vertices[a].Position,raw.Vertices[b].Position)*sourceHeadFactor;if(expected<.005f)continue;
                            Vector3 Point(int v)=>Vector3.Transform(emitted.Vertices[v].Position,inverseHead*pose[poseHead])*report.ProportionScale;
                            Check(Math.Abs(Vector3.Distance(Point(a),Point(b))-expected)<.00005f,"Packaged animated hair was shrunk by the native actor's stature scale");++edges;
                        }
                    }
                }
                Check(edges>100,"Source hair dimension regression did not inspect real triangle edges");
            } else Check(report.SourceParts.Contains("chr/c2750.chrtpfbdt")&&flver.Meshes.Count==18,"Authored Anastacia geometry/textures were not retained");
            Check(report.TextureFallbacks.Any(l=>l.Contains("neutral reflectance approximation")),"DS1R character wrapper did not pass the source profile to material conversion");
            Check(report.Limitations.Any(l=>l.Contains("not ported"))&&report.Limitations.Any(l=>l.Contains("gameplay validation")),"Native approximation/runtime boundary is not reported");
            destination.Models.Enemies.Add(result.Models.Single(m=>m.Name==target));destination.Parts.Enemies.Add(actor);
        }
        var back=MSBB.Read(destination.Write());foreach(var actor in result.Actors)Check(back.Parts.Enemies.Any(a=>a.Name==actor.Name&&a.ModelName==actor.ModelName&&a.CollisionName==collision&&a.NPCParamID==actor.NPCParamID&&a.ThinkParamID==actor.ThinkParamID&&a.Position==actor.Position),"Character model/parameter/collision references failed MSB serialization");
        var newGame=BND4.Read(result.GameParamBytes);Equal(result.Files["param/gameparam/gameparam.parambnd.dcx"],result.GameParamBytes,"Composed parameter file differs from return value");
        foreach(var f in oldGame.Files.Where(f=>Leaf(f.Name)!="NpcParam.param"))Equal(newGame.Files.Single(n=>n.ID==f.ID).Bytes,f.Bytes,"Unrelated or preceding-stage game parameter entry changed");
        foreach(bool think in new[]{false,true}) {
            string name=think?"NpcThinkParam.param":"NpcParam.param";var oldRows=MapCharacters.NativeRows(oldGame.Files.Single(f=>Leaf(f.Name)==name).Bytes,think);var newRows=MapCharacters.NativeRows(newGame.Files.Single(f=>Leaf(f.Name)==name).Bytes,think);
            Check(newRows.Rows.Count==oldRows.Rows.Count+(think?0:3),"Native NPC parameter rows lost/duplicated");
            for(int i=0;i<oldRows.Rows.Count;++i){var a=oldRows.Rows[i];var b=newRows.Rows[i];Check(a.ID==b.ID&&a.Name==b.Name,"Original row ID/order/null name changed");Equal(MapCharacters.Payload(b),MapCharacters.Payload(a),"Original native NPC parameter payload changed");}
            if(think)continue;
            foreach(var report in active) {
                int param=report.NPCParamID!.Value;byte[] original=MapCharacters.Payload(oldRows.Rows.Single(r=>r.ID==803000));byte[] clone=MapCharacters.Payload(newRows.Rows.Single(r=>r.ID==param));
                if(!think){BitConverter.GetBytes(-1).CopyTo(original,4);original[0x149]&=0xDF;foreach(int index in new[]{0x146,0x147,0x14A,0x14B})original[index]=255;Check(BitConverter.ToInt32(clone,4)==-1&&report.ThinkParamID==-1,"Imported NPC AI is not explicitly disabled");Check((clone[0x149]&0x20)==0,"Owned default-language voice bank retained multilingual registration");}Equal(clone,original,"Native NPC/Think donor behavior changed beyond documented clone fields");
            }
        }
        // Unsupported/missing source appearance is an error, never a native head substitution.
        var exhausted=MSBB.Read(mapBytes);
        for(int id=8900;id<=8999;id++)if(!exhausted.Models.Enemies.Any(m=>m.Name=="c"+id))exhausted.Models.Enemies.Add(new MSBB.Model.Enemy("c"+id));
        byte[] exhaustedBefore=exhausted.Write();bool allocationRejected=false;
        try {MapCharacters.BakeFirelink(Read,"DS1R",mapName,defs,nativeFixture.Root,exhausted,offset,collision,stagedGame);}
        catch(InvalidDataException){allocationRejected=true;}
        Check(allocationRejected,"Exhausted character range rolled over to c9000 instead of rejecting");
        Equal(exhausted.Write(),exhaustedBefore,"Exhausted character allocation mutated the destination MSB");
        Reject(()=>MapCharacters.BakeFirelink(Read,"DS3",mapName,defs,nativeRoot,MSBB.Read(mapBytes),offset,collision),"Unsupported profile was silently substituted");
        Reject(()=>MapCharacters.BakeFirelink(Read,"DS1R",mapName,defs,nativeRoot,MSBB.Read(mapBytes),new(float.NaN),collision),"Non-finite character offset accepted");
        Reject(()=>MapCharacters.BakeFirelink(path=>path=="facegen/FaceGen.fgbnd"?null:Read(path),"DS1R",mapName,defs,nativeRoot,MSBB.Read(mapBytes),offset,collision),"Missing source head was silently substituted");
        Reject(()=>MapCharacters.BakeFirelink(Read,"DS1R",mapName,defs,nativeRoot,MSBB.Read(mapBytes),offset,"missing"),"Dangling collision reference accepted");
        foreach(var (path,hash) in hashes)Equal(SHA256.HashData(File.ReadAllBytes(path)),hash,"Original source/native game asset changed");
        string output=Path.Combine(Directory.GetCurrentDirectory(),"out/map-editor/firelink-validation/map-characters-tests.json");Directory.CreateDirectory(Path.GetDirectoryName(output)!);File.WriteAllText(output,JsonSerializer.Serialize(new{actors=active,reports=result.Reports.Count,disabled=result.Reports.Count-3,originalFiles=hashes.Count,gameplayValidated=false},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"Map characters passed: 3 source NPC appearances, {result.Reports.Count-3} explicit unconverted actors; MSB, native animation/skeleton, PS4 textures, opaque composed params and {hashes.Count} original files verified. Gameplay remains unverified.");
    }
}
