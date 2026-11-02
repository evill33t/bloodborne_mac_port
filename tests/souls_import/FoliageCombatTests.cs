// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;
static class FoliageCombatTests {
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Check(bool b,string why){if(!b)throw new Exception(why);}
    static Matrix4x4 Sample(Spline.TransformTrack t,float frame) {
        Vector3 p=t.StaticPosition,s=t.StaticScale;
        if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(frame)??p.X,t.SplinePosition.GetValueY(frame)??p.Y,t.SplinePosition.GetValueZ(frame)??p.Z);
        if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(frame)??s.X,t.SplineScale.GetValueY(frame)??s.Y,t.SplineScale.GetValueZ(frame)??s.Z);
        return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.StaticRotation))*Matrix4x4.CreateTranslation(p);
    }
    static float Error(Matrix4x4 a,Matrix4x4 b) {
        float[] x=[a.M11,a.M12,a.M13,a.M21,a.M22,a.M23,a.M31,a.M32,a.M33,a.M41,a.M42,a.M43],y=[b.M11,b.M12,b.M13,b.M21,b.M22,b.M23,b.M31,b.M32,b.M33,b.M41,b.M42,b.M43];return x.Zip(y,(x,y)=>Math.Abs(x-y)).Max();
    }
    static void Main(string[] args) {
        var hashes=new Dictionary<string,byte[]>();
        byte[] Read(string path){var b=File.ReadAllBytes(path);hashes[path]=SHA256.HashData(b);return b;}
        byte[]? Source(string p)=>File.Exists(Path.Combine(args[0],p))?Read(Path.Combine(args[0],p)):null;byte[] Native(string p)=>Read(Path.Combine(args[1],p));string definitions=args[2];
        var source=MSB1.Read(Source("map/MapStudio/m12_00_00_00.msb")!);var selected=source.Parts.Enemies.Where(p=>FoliageCombat.Select(p,"m12_00_00_00")).ToArray();
        Check(selected.Length==12&&selected.All(p=>!FoliageCombat.Select(p,"m10_01_00_00")),"Ordinary source selection is not bounded to12 Darkroot actors");
        foreach(var p in selected) {
            var copy=(MSB1.Part.Enemy)p.DeepCopy();copy.InitAnimID=9000;Check(!FoliageCombat.Select(copy,"m12_00_00_00"),"Hidden wake actor admitted");
            copy=(MSB1.Part.Enemy)p.DeepCopy();copy.EntityID=1200090;Check(!FoliageCombat.Select(copy,"m12_00_00_00"),"Butterfly helper admitted");
        }
        var destination=new MSBB();var slot=MSBB.Read(Native("map/MapStudio/m24_01_00_01.msb.dcx")).Parts.Collisions.First();
        var additive=BND4.Read(Native("chr/c2050.anibnd.dcx")).Files.Single(f=>f.ID==1000002400);var ap=new NativeAnimation.Pack(additive.Bytes.ToArray());int aa=ap.Object("hkaSplineCompressedAnimation");
        Console.WriteLine($"Native additive contract: frames={ap.Int(aa+56)} dt={ap.Float(aa+80)} duration={ap.Float(aa+16)}");
        var sg=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx")!);var sa=PARAM.Read(sg.Files.Single(f=>Leaf(f.Name)=="AtkParam_Npc.param").Bytes);Console.WriteLine($"Source attacks: type={sa.ParamType} size={sa.DetectedSize} endian={sa.BigEndian}");
        var chunks=selected.GroupBy(p=>p.CollisionName).Select(g=>new MapCollisionBindings.Chunk(g.Key,g.SelectMany(p=>new[]{new MapCollision.Triangle(p.Position+new Vector3(-5,-.1f,-5),p.Position+new Vector3(5,-.1f,-5),p.Position+new Vector3(0,-.1f,5))}).ToArray(),[])).ToArray();
        var slots=chunks.Select((c,i)=>{var s=(MSBB.Part.Collision)slot.DeepCopy();s.Name="h"+i.ToString("D4")+"B0";return s;}).ToArray();
        var result=MapEnemySpawns.Bake(Source,source,"m12_00_00_00",definitions,args[1],destination,Vector3.Zero,chunks,slots,Native("param/gameparam/gameparam.parambnd.dcx"),nativeMap:"m40_04_00_00",readNative:p=>p=="script/m40_04_00_00.luabnd.dcx"?Native("script/m24_01_00_00.luabnd.dcx"):File.Exists(Path.Combine(args[1],p))?Native(p):null,ordinaryFirstEntity:4049000);
        Check(result.Spawns.Count==12&&result.Spawns.Select(p=>p.SourceName).ToHashSet().SetEquals(selected.Select(p=>p.Name)),"Spawn population differs from explicit source placement list");
        Check(result.Spawns.All(p=>p.EntityID is >=4049900 and <4049950&&p.ThinkParamID==SourceEnemyAi.FoliageGoal&&p.ProportionScale==1&&p.DirectWeightShare>.99999),"Owned actor IDs, source stature/skin or AI binding lost");
        Check(result.Skipped.Any(p=>p.SourceName=="c2330_0026"),"Disabled helper was not reported");
        var game=BND4.Read(result.Files["param/gameparam/gameparam.parambnd.dcx"]);var original=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx"));
        PARAM Typed(BND4 b,string name,string def){var p=PARAM.Read(b.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs/"+def+".xml"))));return p;}
        PARAM npc=Typed(game,"NpcParam","NpcParam"),think=Typed(game,"NpcThinkParam","NpcThinkParam"),attack=Typed(game,"AtkParam_Npc","AtkParam"),behavior=Typed(game,"BehaviorParam","BehaviorParam");
        Check(think.Rows.Single(r=>r.ID==SourceEnemyAi.FoliageGoal)["battleGoalID"].Value.Equals(SourceEnemyAi.FoliageGoal),"Owned brain does not select source goal");
        int preservedClips=0,checkedPoses=0;float maxPoseError=0;
        foreach(int owner in new[]{8825,8826}) {
            var n=npc.Rows.Single(r=>r.ID==owner*100);Check(Convert.ToInt32(n["hp"].Value)==129&&Convert.ToInt32(n["behaviorVariationId"].Value)==owner*10&&Convert.ToInt32(n["normalChangeAnimChrId"].Value)==-1,"Owned health, contacts or animation owner lost");
            foreach(int judge in new[]{120,130,150,151,160,161}) {
                var b=behavior.Rows.Single(r=>r.ID==200000000+owner*10000+judge);var a=attack.Rows.Single(r=>r.ID==Convert.ToInt32(b["refId"].Value));
                Check(Convert.ToInt32(a["throwFlag"].Value)==0&&Convert.ToInt32(a["throwTypeId"].Value)==0&&Convert.ToInt32(a["atkAttribute"].Value)==1,"Paired grab or wrong damage attribute retained");
                Check(Convert.ToInt32(a["atkPhys"].Value) is >=120 and <=200&&Convert.ToSingle(a["hit0_Radius"].Value)==.2f,"Source damage/contact radius lost");
                Check(Convert.ToInt32(a["hit0_DmyPoly1"].Value) is 101 or 102&&Convert.ToInt32(a["spEffectId0"].Value)==-1,"Contact uses donor socket or unconverted source effect");
            }
            var bank=BND4.Read(result.Files["chr/c"+owner+".anibnd.dcx"]);var tae=TAE.Read(bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);
            var donorBank=BND4.Read(Native("chr/c2050.anibnd.dcx"));
            foreach(var donorClip in donorBank.Files.Where(f=>f.ID>=1000000000)) {
                var input=new NativeAnimation.Pack(donorClip.Bytes.ToArray());var converted=bank.Files.Single(f=>f.ID==donorClip.ID).Bytes.ToArray();var output=new NativeAnimation.Pack(converted);
                int a=input.Object("hkaSplineCompressedAnimation"),b=output.Object("hkaSplineCompressedAnimation"),nativeTracks=72,total=104;
                Check(input.Int(a+56)==output.Int(b+56)&&input.Float(a+16)==output.Float(b+16)&&input.Float(a+80)==output.Float(b+80),"Native additive/action timing changed");
                var oldData=input.Array(a+152,1,64*1024*1024);var data=output.Array(b+152,1,64*1024*1024);var oldStarts=input.Array(a+88,4,256);var starts=output.Array(b+88,4,256);var ends=input.Array(a+104,4,256);
                for(int block=0;block<oldStarts.Count;block++) {
                    int oldAt=input.Data+oldData.At+input.Int(oldStarts.At+block*4),newAt=output.Data+data.At+output.Int(starts.At+block*4),used=input.Int(ends.At+block*4);
                    Check(donorClip.Bytes.Span.Slice(oldAt,nativeTracks*4).SequenceEqual(converted.AsSpan(newAt,nativeTracks*4))&&donorClip.Bytes.Span.Slice(oldAt+nativeTracks*4,used-nativeTracks*4).SequenceEqual(converted.AsSpan(newAt+total*4,used-nativeTracks*4)),"Original compressed native rig channels changed");
                }
                int action=donorClip.ID-1000000000;if(action is not(2400 or 2401 or 3002 or 3003 or 3005 or 3006)){preservedClips++;continue;}
                var decoded=Spline.ReadSplineCompressedAnimByteBlock(false,converted.AsSpan(output.Data+data.At,data.Count).ToArray(),total,output.Int(b+60));
                var sourceClip=SourceAnimation.Load(Source,"c2330",action is 2400 or 2401?0:action);
                for(int block=0;block<starts.Count;block++) {
                    int last=Math.Min(255,output.Int(b+56)-1-block*255);
                    foreach(int frame in new[]{0,last/2,last}) {
                        var expected=action is 2400 or 2401?Enumerable.Repeat(Matrix4x4.Identity,32).ToArray():sourceClip.Sample((block*255f+frame)/(output.Int(b+56)-1)*sourceClip.Duration);
                        if(action is not(2400 or 2401)){Check(Matrix4x4.Invert(Sample(decoded[block][0],frame),out var inverse),"Singular native root");expected[0]*=inverse;}
                        for(int bone=0;bone<32;bone++){float error=Error(expected[bone],Sample(decoded[block][nativeTracks+bone],frame));maxPoseError=Math.Max(maxPoseError,error);Check(error<.0035f,"Source attack/additive pose changed");}checkedPoses++;
                    }
                }
                preservedClips++;
            }
            Check(tae.Animations.Where(a=>a.Events.Any(e=>e.Type is 1 or 2)).Select(a=>(int)a.ID).ToHashSet().SetEquals(new[]{3002,3003,3005,3006}),"Dormant donor attacks/projectiles remain active");
            foreach(var action in tae.Animations.Where(a=>a.ID is 3002 or 3003 or 3005 or 3006)) {
                Check(action.Events.Where(e=>e.Type is 0 or 1).All(e=>e.Group!=null&&action.EventGroups.Contains(e.Group)),"Imported contact/cancel gate is absent from native action groups");
                Check(action.Events.Where(e=>e.Type==0).GroupBy(e=>e.Group).All(g=>g.Key!.GroupType==0&&g.Select(e=>BitConverter.ToInt32(e.GetParameterBytes(false))).Distinct().Count()==1),"Distinct cancel gates share an animation group");
                Check(action.Events.Where(e=>e.Type==1).All(e=>e.Group!.GroupType==1),"Imported contacts use the wrong native group type");
            }
            foreach(int action in new[]{3005,3006}) {
                var hits=tae.Animations.Single(a=>a.ID==action).Events.Where(e=>e.Type==1).ToArray();
                Check(hits.Length==2&&hits[0].StartTime==hits[1].StartTime&&hits[0].EndTime==hits[1].EndTime&&hits.Select(e=>BitConverter.ToInt32(e.GetParameterBytes(false),4)).Order().SequenceEqual(new[]{0,1}),"Simultaneous left/right claws collapsed into sequential or shared-index hits");
            }
            var model=FLVER2.Read(BND4.Read(result.Files["chr/c"+owner+".chrbnd.dcx"]).Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);var world=SkinRetarget.World(model.Nodes);
            var authored=FLVER2.Read(MapCharacters.Authored(Source,"c2330").Model);var sourceWorld=SkinRetarget.World(authored.Nodes);
            for(int i=0;i<authored.Meshes.Count;i++) {
                bool cutout=Leaf(authored.Materials[authored.Meshes[i].MaterialIndex].MTD)=="C_DullLeather[DSB]_Edge_Spec.mtd";
                Check(Leaf(model.Materials[model.Meshes[i].MaterialIndex].MTD)==(cutout?"C[ARSN]_e.mtd":"C[ARSN].mtd"),"Foliage cutout became solid leaf rectangles or trunk became translucent");
            }
            foreach(var d in model.Dummies.Where(d=>d.ReferenceID is >=101 and <=122)) {
                var old=authored.Dummies.Single(x=>x.ReferenceID==d.ReferenceID-100);var before=old.ParentBoneIndex<0?Matrix4x4.Identity:sourceWorld[old.ParentBoneIndex];
                Check(Vector3.Distance(Vector3.Transform(old.Position,before),Vector3.Transform(d.Position,world[d.ParentBoneIndex]))<.0001f&&model.Nodes[d.ParentBoneIndex].Name.StartsWith("DS1_",StringComparison.Ordinal),"Contact rest frame moved or uses native donor hand");
            }
        }
        foreach(string table in new[]{"NpcParam","NpcThinkParam","BehaviorParam","AtkParam_Npc"}) {
            var originalBytes=original.Files.Single(f=>Leaf(f.Name)==table+".param").Bytes;var newBytes=game.Files.Single(f=>Leaf(f.Name)==table+".param").Bytes;
            var before=PARAM.Read(originalBytes);var after=PARAM.Read(newBytes);var def=new PARAMDEF{ParamType=before.ParamType,DataVersion=before.ParamdefDataVersion};def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=(int)before.DetectedSize});before.ApplyParamdef(def);after.ApplyParamdef(def);
            var occurrences=new Dictionary<int,int>();var updatedRows=after.Rows.ToLookup(r=>r.ID);
            foreach(var old in before.Rows) {
                int occurrence=occurrences.GetValueOrDefault(old.ID);occurrences[old.ID]=occurrence+1;
                var updated=updatedRows[old.ID].ElementAt(occurrence);
                Check(ImportParam.Payload(old).AsSpan().SequenceEqual(ImportParam.Payload(updated)),"Original native row changed: "+table+"/"+old.ID);
            }
        }
        foreach(var (path,hash) in hashes)Check(SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(hash),"Original game asset changed");
        if(args.Length>3){
            Directory.CreateDirectory(args[3]);
            foreach(var (path,bytes) in result.Files.Where(p=>p.Key.StartsWith("chr/",StringComparison.Ordinal))) {
                string output=Path.Combine(args[3],path);Directory.CreateDirectory(Path.GetDirectoryName(output)!);File.WriteAllBytes(output,bytes);
            }
            File.WriteAllText(Path.Combine(args[3],"validation.json"),JsonSerializer.Serialize(new{preservedClips,checkedPoses,maxPoseError,spawns=result.Spawns.Select(p=>new{p.SourceName,p.TargetModel,p.EntityID,p.ThinkParamID}),originalHashes=hashes.ToDictionary(p=>p.Key,p=>Convert.ToHexStringLower(p.Value))},new JsonSerializerOptions{WriteIndented=true}));
        }
        Console.WriteLine($"PASS:12 bounded foliage, two owned rigs, four claw actions, simultaneous contact indices, source damage/sockets, owned source goal; {preservedClips} unchanged native clip channels, {checkedPoses} independently decoded source/additive poses, max error{maxPoseError}, {hashes.Count} original asset hashes");
    }
}
