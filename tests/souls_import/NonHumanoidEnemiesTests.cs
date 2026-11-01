// SPDX-License-Identifier: GPL-3.0-or-later
#nullable enable
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;
static class NonHumanoidEnemiesTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}throw new Exception(message);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    // Canonical node-name order lets a byte comparison cover every serialized
    // render record while treating indices as references to named nodes.
    static void Permute(FLVER2 model,IEnumerable<string> names) {
        var original=model.Nodes.ToArray();var byName=names.Select((name,index)=>(name,index)).ToDictionary(x=>x.name,x=>x.index);int Remap(int i)=>i<0?i:byName[original[i].Name];short Link(short i)=>checked((short)Remap(i));
        foreach(var n in original){n.ParentIndex=Link(n.ParentIndex);n.FirstChildIndex=Link(n.FirstChildIndex);n.NextSiblingIndex=Link(n.NextSiblingIndex);n.PreviousSiblingIndex=Link(n.PreviousSiblingIndex);}
        model.Nodes=original.OrderBy(n=>byName[n.Name]).ToList();foreach(var d in model.Dummies){d.ParentBoneIndex=Link(d.ParentBoneIndex);d.AttachBoneIndex=Link(d.AttachBoneIndex);}
        foreach(var m in model.Meshes){m.NodeIndex=Remap(m.NodeIndex);if(m.BoneIndices.Count>0)m.BoneIndices=m.BoneIndices.Select(Remap).ToList();else foreach(var v in m.Vertices){if(m.UseBoneWeights)for(int k=0;k<4;k++)v.BoneIndices[k]=Remap(v.BoneIndices[k]);else v.NormalW=Remap(v.NormalW);}}
    }
    static byte[] Canonical(FLVER2 model){var copy=FLVER2.Read(model.Write());Permute(copy,copy.Nodes.Select(n=>n.Name).Order(StringComparer.Ordinal));return copy.Write();}
    static void CheckRenderOrder(FLVER2 model) {
        int active=model.Nodes.Count(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)==0);
        Check(model.Nodes.Take(active).All(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)==0)&&model.Nodes.Skip(active).All(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)!=0),"Native active-node prefix/disabled-tail contract was lost");
        foreach(var mesh in model.Meshes)foreach(var v in mesh.Vertices)for(int k=0;k<4;k++)if(v.BoneWeights[k]>0)Check(SkinRetarget.Bone(mesh,v.BoneIndices[k])<active,"Skin exceeds the native active render palette");
    }
    // Inspect serialized fixups independently of NativeAnimation.Pack before
    // decoding poses. hkStringPtr's low bit is ownership, not string data.
    sealed class RawStrings {
        readonly byte[] bytes;readonly Dictionary<int,int> pointers=new();readonly Dictionary<string,int> objects=new();
        internal readonly int Data;
        internal RawStrings(byte[] bytes) {
            this.bytes=bytes;int section=64+BitConverter.ToUInt16(bytes,62),classes=BitConverter.ToInt32(bytes,section+20),header=section+128;
            Data=BitConverter.ToInt32(bytes,header+20);int local=BitConverter.ToInt32(bytes,header+24),global=BitConverter.ToInt32(bytes,header+28),virtuals=BitConverter.ToInt32(bytes,header+32),end=BitConverter.ToInt32(bytes,header+36);
            for(int at=local;at+8<=global;at+=8)if(Int(at)!=-1)pointers.Add(Int(at),Int(at+4));
            for(int at=virtuals;at+12<=end;at+=12)if(Int(at)!=-1){int text=classes+Int(at+8),stop=Array.IndexOf(bytes,(byte)0,text);objects.Add(System.Text.Encoding.ASCII.GetString(bytes,text,stop-text),Int(at));}
        }
        internal int Int(int at)=>BitConverter.ToInt32(bytes,Data+at);
        internal int Object(string name)=>objects[name];
        internal int Pointer(int field)=>pointers[field];
        internal byte[] Text(int field) {
            int at=Pointer(field);Check(((Data+at)&1)==0,"Borrowed hkStringPtr has heap-owned low bit at "+field);
            Check(BitConverter.ToUInt64(bytes,Data+field)==0,"Serialized borrowed string pointer is not a zero fixup placeholder");
            int stop=Array.IndexOf(bytes,(byte)0,Data+at);return bytes.AsSpan(Data+at,stop-Data-at).ToArray();
        }
        internal int[] Bones(){int obj=Object("hkaSkeleton"),at=Pointer(obj+40);return Enumerable.Range(0,Int(obj+48)).Select(i=>at+i*16).ToArray();}
    }
    static Matrix4x4 Sample(Spline.TransformTrack t,float time) {
        Vector3 p=t.StaticPosition,s=t.StaticScale;
        if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(time)??p.X,t.SplinePosition.GetValueY(time)??p.Y,t.SplinePosition.GetValueZ(time)??p.Z);
        if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(time)??s.X,t.SplineScale.GetValueY(time)??s.Y,t.SplineScale.GetValueZ(time)??s.Z);
        var q=t.HasSplineRotation?t.SplineRotation.GetValue(time):t.StaticRotation;
        return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(p);
    }
    static float Error(Matrix4x4 a,Matrix4x4 b) {
        float[] x=[a.M11,a.M12,a.M13,a.M21,a.M22,a.M23,a.M31,a.M32,a.M33,a.M41,a.M42,a.M43];float[] y=[b.M11,b.M12,b.M13,b.M21,b.M22,b.M23,b.M31,b.M32,b.M33,b.M41,b.M42,b.M43];return x.Zip(y,(x,y)=>Math.Abs(x-y)).Max();
    }
    public static void Main(string[] args) {
        string source=args[0],native=args[1],defs=args[2];var originals=new Dictionary<string,byte[]>();
        byte[] ReadFile(string p){var b=File.ReadAllBytes(p);originals.TryAdd(p,SHA256.HashData(b));return b;}
        byte[]? Source(string p)=>File.Exists(Path.Combine(source,p))?ReadFile(Path.Combine(source,p)):null;
        byte[] Native(string p)=>ReadFile(Path.Combine(native,p));
        var sourceMap=MSB1.Read(Source("map/MapStudio/m10_01_00_00.msb")!);var sourceGame=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx")!);var sourceRows=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);sourceRows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,"NpcParam.xml")));
        // Primary bytecode regression: the old textual disassembler lost IF_Z.
        // In the shipped shader, workflow0 enters the packed PBR branch;
        // authored workflow1 takes ELSE legacy RGB specular/alpha roughness.
        var sourceShaders=BND3.Read(Source("shader/FRPG_FlverPBL_fpo_DX11.shaderbnd.dcx")!);var shader=sourceShaders.Files.Single(f=>Leaf(f.Name)=="FRPG_Phn_DifSpcBmp__________HemEnv.fpo").Bytes.ToArray();
        int dxbc=shader.AsSpan().IndexOf("DXBC"u8);Check(dxbc>=0,"Source shader DXBC missing");int shex=-1;
        for(int i=0;i<BitConverter.ToInt32(shader,dxbc+28);i++){int at=dxbc+BitConverter.ToInt32(shader,dxbc+32+i*4);if(shader.AsSpan(at,4).SequenceEqual("SHEX"u8))shex=at+8;}
        Check(shex>=0&&BitConverter.ToUInt32(shader,shex+0x09f0)==0x0400001f&&BitConverter.ToUInt32(shader,shex+0x09f4)==0x0020800a&&BitConverter.ToInt32(shader,shex+0x09f8)==0&&BitConverter.ToInt32(shader,shex+0x09fc)==85,"Source MaterialWorkflow branch is not the verified IF_Z(cb0[85].x)");
        var result=new List<object>();int checkedClips=0,checkedPoses=0;float maxPoseError=0;
        foreach(var profile in new[]{NonHumanoidEnemies.Boar,NonHumanoidEnemies.Demon,NonHumanoidEnemies.Rat}) {
            var actor=sourceMap.Parts.Enemies.First(a=>a.ModelName==profile.Source);Check(NonHumanoidEnemies.Select(actor,"m10_01_00_00")==profile&&NonHumanoidEnemies.Select(actor,"m10_02_00_00")==null,"Nonhumanoid source contract is not explicit");
            if(profile==NonHumanoidEnemies.Rat) {
                var firelink=MSB1.Read(Source("map/MapStudio/m10_02_00_00.msb")!);var aqueduct=firelink.Parts.Enemies.Single(p=>p.ModelName=="c1201");
                Check(NonHumanoidEnemies.Select(aqueduct,"m10_02_00_00")==profile&&sourceMap.Parts.Enemies.Count(p=>NonHumanoidEnemies.Select(p,"m10_01_00_00")==profile)==3,"Source rat population is not exactly aqueduct plus three bridge rats");
                foreach(int change in Enumerable.Range(0,5)) {var foreign=(MSB1.Part.Enemy)actor.DeepCopy();if(change==0)foreign.ModelName="c1200";if(change==1)foreign.ThinkParamID=120102;if(change==2)foreign.InitAnimID=9000;if(change==3)foreign.TalkID=100;if(change==4)foreign.NPCParamID=120102;Check(NonHumanoidEnemies.Select(foreign,"m10_01_00_00")==null,"Unsupported rat or state was admitted");}
                Reject(()=>NonHumanoidEnemies.SourceSlot(profile,9000000),"Unowned native rat bank accepted");Reject(()=>NonHumanoidEnemies.SourceSlot(profile,9999),"Unknown native rat state accepted");
            }
            var row=sourceRows.Rows.Single(r=>r.ID==actor.NPCParamID);var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="modelDispMask"+i).Value)!=0).ToHashSet();
            var files=new Dictionary<string,byte[]>();string target=profile==NonHumanoidEnemies.Rat?"c8821":profile==NonHumanoidEnemies.Boar?"c8801":"c8800";
            var skin=NonHumanoidEnemies.Package(Source,Native,profile,target,masks,files);Check(skin.DirectWeightShare>=.999999&&skin.ProportionScale==1,"Source body relies on native proportions or actor scaling");
            if(profile==NonHumanoidEnemies.Boar) {
                var owned=TPF.Read(skin.Textures.Write());var specular=skin.Model.Materials.Select(m=>Leaf(m.Textures.Single(t=>t.ParamName=="g_SpecularTexture").Path).Split('.')[0]).ToArray();
                Check(specular.All(n=>n.StartsWith(target+"_source_f0_",StringComparison.Ordinal))&&specular.Distinct().Count()==5,"Boar source reflection was discarded or duplicate inputs were not shared");
                foreach(string name in specular.Distinct()){var texture=owned.Textures.Single(t=>t.Name==name);var dds=texture.HeaderizeExt(out _);Check((int)new DDS(dds).GetDXGIFormat()==28&&SkinRetarget.DdsMipImages(dds)[0].subImages[0].Where((_,i)=>i%4!=3).Any(x=>x>0),"Boar reflection is missing, black or gamma-tagged");}
                var shininess=skin.Model.Materials.Select(m=>Leaf(m.Textures.Single(t=>t.ParamName=="g_ShininessTexture").Path).Split('.')[0]).ToArray();Check(shininess.All(n=>n.StartsWith(target+"_source_shine_",StringComparison.Ordinal))&&shininess.Distinct().Count()==2,"Source boar alpha roughness was left in RGB-packed material form");
                Check(skin.Model.Materials.Where(m=>m.MTD.EndsWith("_e.mtd",StringComparison.Ordinal)).Count()==2,"Source boar edge materials lost alpha testing");
                var sourceTextures=MapCharacters.Authored(Source,profile.Source).Textures;
                foreach(string packed in new[]{"c3469_1_s","c3469_2_s"}) {
                    var originalPacked=SkinRetarget.DdsMipImages(sourceTextures.Single(t=>t.Name==packed).HeaderizeExt(out _))[0].subImages;var nativePacked=SkinRetarget.DdsMipImages(owned.Textures.Single(t=>t.Name==target+"_import_"+packed).HeaderizeExt(out _))[0].subImages;
                    Check(originalPacked.Count==nativePacked.Count&&originalPacked.Zip(nativePacked).All(p=>p.First.SequenceEqual(p.Second)),"Boar fix changed source shininess/material mip payloads");
                }
            } else Check(skin.Model.Materials.All(m=>m.Textures.Single(t=>t.ParamName=="g_SpecularTexture").Path.EndsWith("SYSTEX_DummyReflectance.tga",StringComparison.Ordinal)),"Boar material policy affected the Titanite Demon");
            CheckRenderOrder(FLVER2.Read(skin.Model.Write()));var permuted=FLVER2.Read(skin.Model.Write());Permute(permuted,permuted.Nodes.Select(n=>n.Name).Reverse());var canonical=Canonical(permuted);var namedWorld=permuted.Nodes.Select((n,i)=>(n.Name,world:SkinRetarget.World(permuted.Nodes)[i])).ToDictionary(x=>x.Name,x=>x.world);
            NonHumanoidEnemies.OrderRenderNodes(permuted);CheckRenderOrder(permuted);Check(canonical.AsSpan().SequenceEqual(Canonical(permuted)),"Render permutation changed serialized named node/vertex/mesh/palette/dummy/geometry records");var orderedWorld=SkinRetarget.World(permuted.Nodes);
            Check(permuted.Nodes.Select((n,i)=>namedWorld[n.Name]==orderedWorld[i]).All(x=>x),"Render permutation changed a named bind matrix");
            var authored=MapCharacters.Authored(Source,profile.Source);var original=FLVER2.Read(authored.Model);var sourceMeshes=original.Meshes.Where(m=>{var match=System.Text.RegularExpressions.Regex.Match(original.Materials[m.MaterialIndex].Name,@"#(\d+)#");return !match.Success||masks.Contains(int.Parse(match.Groups[1].Value));}).ToArray();
            Check(sourceMeshes.Length==skin.Model.Meshes.Count,"Source equipment mesh selection changed");
            float maxVertexError=0;for(int mesh=0;mesh<sourceMeshes.Length;mesh++)for(int v=0;v<sourceMeshes[mesh].Vertices.Count;v++)maxVertexError=Math.Max(maxVertexError,Vector3.Distance(sourceMeshes[mesh].Vertices[v].Position,skin.Model.Meshes[mesh].Vertices[v].Position));
            Check(maxVertexError<.0001f,"Source body geometry was stretched: "+maxVertexError);
            var before=BND4.Read(Native("chr/"+profile.Native+".anibnd.dcx"));var after=BND4.Read(files["chr/"+target+".anibnd.dcx"]);
            Check(before.Files.Count==after.Files.Count,"Native playback binder membership changed");
            var rawOld=new RawStrings(before.Files.Single(f=>f.ID==1000000).Bytes.ToArray());var rawNew=new RawStrings(after.Files.Single(f=>f.ID==1000000).Bytes.ToArray());var oldBoneFields=rawOld.Bones();var newBoneFields=rawNew.Bones();
            Check(rawOld.Text(rawOld.Object("hkaSkeleton")+16).SequenceEqual(rawNew.Text(rawNew.Object("hkaSkeleton")+16)),"Native skeleton name changed");
            for(int i=0;i<oldBoneFields.Length;i++)Check(rawOld.Pointer(oldBoneFields[i])==rawNew.Pointer(newBoneFields[i])&&rawOld.Text(oldBoneFields[i]).SequenceEqual(rawNew.Text(newBoneFields[i])),"Native borrowed bone name/target changed");
            var rawSource=SourceAnimation.Load(Source,profile.Source,0);for(int i=0;i<rawSource.Bones.Count;i++)Check(rawNew.Text(newBoneFields[oldBoneFields.Length+i]).SequenceEqual(System.Text.Encoding.UTF8.GetBytes("DS1_"+rawSource.Bones[i].Name)),"Source borrowed bone name changed");
            var rig=NativeAnimation.Skeleton(after.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),allowMirroredScale:true);var oldRig=NativeAnimation.Skeleton(before.Files.Single(f=>f.ID==1000000).Bytes.ToArray());var idle=rawSource;
            var plan=NonHumanoidEnemies.Rig(authored.Model,BND4.Read(Native("chr/"+profile.Native+".chrbnd.dcx")).Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray(),before.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),idle);
            Reject(()=>NativeRigAppend.Create(plan.Model,before.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),[plan.Added[0] with{Parent=999}]),"Out-of-range appended bone parent accepted");
            Reject(()=>NonHumanoidEnemies.SourceSlot(profile,9999),"Unknown native playback slot accepted");
            var bad=before.Files.First(f=>f.ID>=1000000000).Bytes.ToArray();var bp=new NativeAnimation.Pack(bad);bad[bp.Data+bp.Object("hkaAnimationBinding")+80]=2;
            Reject(()=>NativeRigAppend.Clip(bad,plan,_=>plan.Added.Select(b=>b.ReferenceLocal).ToArray()),"Unknown additive blend contract accepted");
            Check(rig.Names.Length==profile.NativeBones+idle.Bones.Count&&rig.Names.Take(oldRig.Names.Length).SequenceEqual(oldRig.Names)&&rig.Local.Take(oldRig.Local.Length).SequenceEqual(oldRig.Local),"Native bone indices/reference pose changed");
            Check(rig.Names.Skip(oldRig.Names.Length).SequenceEqual(idle.Bones.Select(b=>"DS1_"+b.Name)),"Source joints were lost or renamed inconsistently");Matrix4x4.Invert(oldRig.Local[0],out var inverse);
            var cache=new Dictionary<int,SourceAnimation>();
            foreach(var old in before.Files.Where(f=>f.ID>=1000000000)) {
                var converted=after.Files.Single(f=>f.ID==old.ID).Bytes.ToArray();var oldStrings=new RawStrings(old.Bytes.ToArray());var newStrings=new RawStrings(converted);
                Check(oldStrings.Text(oldStrings.Object("hkaAnimationBinding")+16).SequenceEqual(newStrings.Text(newStrings.Object("hkaAnimationBinding")+16)),"Native animation binding borrowed string changed");
                var op=new NativeAnimation.Pack(old.Bytes.ToArray());var np=new NativeAnimation.Pack(converted);int oa=op.Object("hkaSplineCompressedAnimation"),na=np.Object("hkaSplineCompressedAnimation");
                Check(op.Float(oa+16)==np.Float(na+16)&&op.Int(oa+56)==np.Int(na+56)&&op.Float(oa+80)==np.Float(na+80),"Native action duration or frame timing changed");
                var od=op.Array(oa+152,1,64*1024*1024);var nd=np.Array(na+152,1,64*1024*1024);var os=op.Array(oa+88,4,256);var ns=np.Array(na+88,4,256);var oe=op.Array(oa+104,4,256);
                int tracks=oldRig.Names.Length,total=rig.Names.Length;
                var decoded=Spline.ReadSplineCompressedAnimByteBlock(false,converted.AsSpan(np.Data+nd.At,nd.Count).ToArray(),total,np.Int(na+60));
                int sourceID=NonHumanoidEnemies.SourceSlot(profile,old.ID-1000000000);if(!cache.TryGetValue(sourceID,out var clip))cache.Add(sourceID,clip=SourceAnimation.Load(Source,profile.Source,sourceID));
                for(int block=0;block<os.Count;block++) {
                    int o=op.Data+od.At+op.Int(os.At+block*4),n=np.Data+nd.At+np.Int(ns.At+block*4),used=op.Int(oe.At+block*4);
                    Check(old.Bytes.Span.Slice(o,tracks*4).SequenceEqual(converted.AsSpan(n,tracks*4))&&old.Bytes.Span.Slice(o+tracks*4,used-tracks*4).SequenceEqual(converted.AsSpan(n+total*4,used-tracks*4)),"Original compressed native body channels changed");
                    int end=Math.Min(255,op.Int(oa+56)-1-block*255);
                    foreach(int frame in new[]{0,end/2,end}) {
                        var expected=old.Bytes.Span[op.Data+op.Object("hkaAnimationBinding")+80]==1?Enumerable.Repeat(Matrix4x4.Identity,clip.Bones.Count).ToArray():clip.Sample((block*255f+frame)/(op.Int(oa+56)-1)*clip.Duration);if(old.Bytes.Span[op.Data+op.Object("hkaAnimationBinding")+80]==0){Check(Matrix4x4.Invert(Sample(decoded[block][0],frame),out var rootInverse),"Native root is singular");expected[0]*=rootInverse;}
                        for(int bone=0;bone<expected.Length;bone++){float e=Error(expected[bone],Sample(decoded[block][tracks+bone],frame));maxPoseError=Math.Max(maxPoseError,e);Check(e<.0035f,"Authored source pose mismatch: "+profile.Source+"/"+sourceID+"/"+bone+" native="+old.ID+" frame="+frame+" blend="+old.Bytes.Span[op.Data+op.Object("hkaAnimationBinding")+80]+"="+e);}checkedPoses++;
                    }
                }
                // Existing animation and locomotion bytes remain in place; no
                // original data is removed when the appended arrays are added.
                int oldData=op.Data;Check(old.Bytes.Span.Slice(oldData,16).SequenceEqual(converted.AsSpan(oldData,16)),"Native root container data changed");checkedClips++;
            }
            var targetMap=new MSBB();var donorMap=MSBB.Read(Native("map/MapStudio/m24_01_00_01.msb.dcx"));var slot=donorMap.Parts.Collisions.First();var minimal=MSB1.Read(Source("map/MapStudio/m10_01_00_00.msb")!);minimal.Parts.Enemies.RemoveAll(a=>a.Name!=actor.Name);
            var p=actor.Position-new Vector3(0,.1f,0);MapCollision.Triangle floor=new(p+new Vector3(-2,0,-2),p+new Vector3(2,0,-2),p+new Vector3(0,0,2));
            var imported=MapEnemySpawns.Bake(Source,minimal,"m10_01_00_00",defs,native,targetMap,Vector3.Zero,[new(actor.CollisionName,[floor],[])],[slot],Native("param/gameparam/gameparam.parambnd.dcx"),nativeMap:"m40_03_00_00",readNative:path=>path=="script/m40_03_00_00.luabnd.dcx"?Native("script/m24_01_00_00.luabnd.dcx"):File.Exists(Path.Combine(native,path))?Native(path):null,ordinaryFirstEntity:4039000);
            var spawn=imported.Spawns.Single();
            var game=BND4.Read(imported.Files["param/gameparam/gameparam.parambnd.dcx"]);var rows=MapCharacters.NativeRows(game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);var payload=MapCharacters.Payload(rows.Rows.Single(r=>r.ID==spawn.NPCParamID));
            var expectedPayload=NonHumanoidEnemies.Capsule(MapCharacters.Payload(rows.Rows.Single(r=>r.ID==profile.Npc)),row,defs);foreach(int byteIndex in new[]{0x146,0x147,0x14A,0x14B})expectedPayload[byteIndex]=255;
            Check(payload.SequenceEqual(expectedPayload),"Nonhumanoid NPC parameters changed beyond source capsule and display masks");
            Check(BitConverter.ToSingle(payload,16)==Convert.ToSingle(row.Cells.Single(c=>c.Def.InternalName=="hitHeight").Value)&&BitConverter.ToSingle(payload,20)==Convert.ToSingle(row.Cells.Single(c=>c.Def.InternalName=="hitRadius").Value),"Source capsule dimensions were lost");
            Check(spawn.EntityID==(profile==NonHumanoidEnemies.Rat?4039950:4039900),"Special enemies shifted the existing hollow entity range");Check(spawn.NativeAdapter==profile.Native&&spawn.Position==actor.Position&&spawn.ThinkParamID==profile.Think,"Native actor wrapper/placement is wrong");var ai=BND4.Read(imported.Files["script/m40_03_00_00.luabnd.dcx"]);var goals=LUAINFO.Read(ai.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo")).Bytes);int goal=int.Parse(profile.Native[1..])*100;
            Check(ai.Files.Any(f=>Leaf(f.Name).StartsWith(goal.ToString(),StringComparison.Ordinal)&&Leaf(f.Name).EndsWith(".lua"))&&goals.Goals.Any(g=>g.ID==goal),"Donor combat Lua/goal registry is missing");
            result.Add(new{profile.Source,profile.Native,sourceBones=idle.Bones.Count,nativeBones=profile.NativeBones,totalBones=rig.Names.Length,clips=profile.Clips,maxVertexError,sourceClips=cache.Keys.Order(),capsule=new{height=BitConverter.ToSingle(payload,16),radius=BitConverter.ToSingle(payload,20),offset=BitConverter.ToSingle(payload,28)},spawn});Console.WriteLine($"PASS {profile.Source}: {rig.Names.Length} bones, {profile.Clips} native/source animation pairs, unchanged source geometry and native combat channels");
        }
        foreach(var (path,hash) in originals)Check(SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(hash),"Original game file changed: "+path);
        Directory.CreateDirectory(args[3]);File.WriteAllText(Path.Combine(args[3],"validation.json"),JsonSerializer.Serialize(new{checkedClips,checkedPoses,maxPoseError,originalHashes=originals.ToDictionary(p=>p.Key,p=>Convert.ToHexString(p.Value)),result},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS: {checkedClips} clips / {checkedPoses} independently decoded source poses; {originals.Count} original hashes unchanged");
    }
}
