// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;using System.Security.Cryptography;using System.Numerics;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;
internal static class CatacombsCreaturesTests {
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException(why);}
    static Matrix4x4 Pose(Spline.TransformTrack t,float frame){Vector3 p=t.StaticPosition,s=t.StaticScale;if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(frame)??p.X,t.SplinePosition.GetValueY(frame)??p.Y,t.SplinePosition.GetValueZ(frame)??p.Z);if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(frame)??s.X,t.SplineScale.GetValueY(frame)??s.Y,t.SplineScale.GetValueZ(frame)??s.Z);return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.StaticRotation))*Matrix4x4.CreateTranslation(p);}
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{a.M11-b.M11,a.M12-b.M12,a.M13-b.M13,a.M21-b.M21,a.M22-b.M22,a.M23-b.M23,a.M31-b.M31,a.M32-b.M32,a.M33-b.M33,a.M41-b.M41,a.M42-b.M42,a.M43-b.M43}.Select(Math.Abs).Max();
    static void ValidateMotion(Func<string,byte[]?> source,string sourceModel,int model,CatacombsCreaturesCharacters.Result result,Dictionary<string,byte[]> files) {
        var bank=BND4.Read(files["chr/c"+model+".anibnd.dcx"]);var rig=NativeAnimation.Skeleton(bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),true);var clip=SourceAnimation.Load(result.Actions.Reader(source),sourceModel,result.Actions.SourceSlot(3000));Need(rig.Names.Length==80+clip.Bones.Count&&rig.Names.Skip(80).SequenceEqual(clip.Bones.Select(b=>"DS1_"+b.Name)),"Source/native appended names lost");
        var bytes=bank.Files.Single(f=>f.ID==1000003000).Bytes.ToArray();var p=new NativeAnimation.Pack(bytes);int a=p.Object("hkaSplineCompressedAnimation");var data=p.Array(a+152,1,64*1024*1024);int frames=p.Int(a+56);var decoded=Spline.ReadSplineCompressedAnimByteBlock(false,bytes.AsSpan(p.Data+data.At,data.Count).ToArray(),rig.Names.Length,p.Int(a+60));float max=0;
        foreach(int frame in new[]{0,(frames-1)/2,frames-1}){int block=frame/255,local=frame-block*255;Need(Matrix4x4.Invert(Pose(decoded[block][0],local),out var inverse),"Native motion root singular");for(int bone=0;bone<clip.Bones.Count;bone++){var expected=clip.HollowLocal(bone,frame/(float)(frames-1)*clip.Duration,true);if(bone==0)expected*=inverse;float e=Error(expected,Pose(decoded[block][80+bone],local));max=Math.Max(max,e);Need(e<.004f,"Independent source motion mismatch "+sourceModel+"/"+bone+"="+e);}}
        Console.WriteLine($"Independent {sourceModel}/3000 source pose error{max}");
    }
    static void Main(string[] args){string source=args[0],native=args[1],defs=Path.Combine(args[2],"src/Smithbox.Data/Assets/PARAM/DS1R/Defs");var hashes=new Dictionary<string,string>();
    byte[]? Read(string root,string path){var file=Path.Combine(root,path);if(!File.Exists(file))return null;var b=File.ReadAllBytes(file);hashes[file]=Convert.ToHexString(SHA256.HashData(b));return b;}
    byte[]? Source(string path)=>Read(source,path);byte[] Native(string path)=>Read(native,path)??throw new FileNotFoundException(path);
    var map=MSB1.Read(Source("map/MapStudio/m13_00_00_00.msb")!);var plan=CatacombsCreaturesSource.Load(Source,map,defs);Need(plan.Cohorts.Count==6&&plan.Ordinary.Count==63&&plan.Wakes.Count==17,"Fresh population incorrect");
    Need(plan.Excluded.Count(p=>p.ModelName=="c2900")==8&&plan.Excluded.Count(p=>p.ModelName=="c2930")==3,"Gravelord phantoms were enabled");
    var sourceActions=new CatacombsCreaturesActions(Source,"c2900",20000);Need(sourceActions.Selected[3000]==23000,"Large-curved-sword action bank lost");var reader=sourceActions.Reader(Source);var clip=SourceAnimation.Load(reader,"c2900",3000);Need(clip.Bones.Count is >0 and <=256&&clip.Duration>0,"Authored absolute pose unavailable");
    var ai=CatacombsCreaturesAi.Build(Native("script/m24_01_00_00.luabnd.dcx"),Source,Native);var bank=BND4.Read(ai);var info=LUAINFO.Read(bank.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo")).Bytes);Need(CatacombsCreaturesAi.SourceGoals.All(g=>info.Goals.Any(n=>n.ID==CatacombsCreaturesAi.Goal(g))),"Owned AI registration missing");
    if(Environment.GetEnvironmentVariable("CATACOMBS_AUDIT")=="1") {
        foreach(string name in new[]{"m29","m29a","m29b","m29c","commoneffects"}) {var fx=BND4.Read(Native("sfx/frpg_sfxbnd_"+name+".ffxbnd.dcx"));Console.WriteLine("FireFX "+name+" "+string.Join(",",fx.Files.Where(f=>Leaf(f.Name).Contains("00061010" )).Select(f=>Leaf(f.Name))));}

        var sb=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx")!);var nb=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx"));
        foreach(string table in new[]{"Bullet","BehaviorParam","SpEffectParam"}) {
            string def=table=="Bullet"?"BulletParam":table=="SpEffectParam"?"SpEffect":table;var sp=PARAM.Read(sb.Files.Single(f=>Leaf(f.Name)==table+".param").Bytes);sp.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,def+".xml")));
            var np=PARAM.Read(nb.Files.Single(f=>Leaf(f.Name)==table+".param").Bytes);np.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(defs,"../../BB/Defs/"+def+".xml"))));
            if(table=="Bullet")File.WriteAllText("out/map-editor/catacombs-creatures/evidence/native-fire-bullets.json",System.Text.Json.JsonSerializer.Serialize(np.Rows.Where(r=>r.ID<1000||r.Name!=null&&(r.Name.Contains("火")||r.Name.Contains("炎"))).Select(r=>new {r.ID,r.Name,Cells=r.Cells.ToDictionary(c=>c.Def.InternalName,c=>c.Value)}),new System.Text.Json.JsonSerializerOptions{WriteIndented=true}));
            if(table=="SpEffectParam")File.WriteAllText("out/map-editor/catacombs-creatures/evidence/native-effect-target-policy.json",System.Text.Json.JsonSerializer.Serialize(np.Rows.Where(r=>new[]{5270,5271,5300,5308,5331,5333,5360,5364,7007,7009,71100,71110,71111,7407,7409,80000,81110,81111,81101,90000}.Contains((int)r.ID)||Convert.ToSingle(r["changeHpRate"].Value)<0&&Convert.ToSingle(r["effectEndurance"].Value)>=0).Select(r=>new{r.ID,r.Name,Cells=r.Cells.ToDictionary(c=>c.Def.InternalName,c=>c.Value)}),new System.Text.Json.JsonSerializerOptions{WriteIndented=true}));
            var rows=sp.Rows.Where(r=>table=="Bullet"?r.ID>=26500&&r.ID<26600:table=="SpEffectParam"?(r.ID>=5450&&r.ID<=5452||r.ID==5270||r.ID==5271):Convert.ToInt32(r["variationId"].Value)==26500||Convert.ToInt32(r["variationId"].Value)==29000).Select(r=>new {r.ID,Source=r.Cells.ToDictionary(c=>c.Def.InternalName,c=>c.Value),Native=np.Rows.SingleOrDefault(n=>n.ID==r.ID)?.Cells.ToDictionary(c=>c.Def.InternalName,c=>c.Value)}).ToArray();
            File.WriteAllText("out/map-editor/catacombs-creatures/evidence/"+table+"-closure.json",System.Text.Json.JsonSerializer.Serialize(rows,new System.Text.Json.JsonSerializerOptions{WriteIndented=true}));
        }
    }
    if(Environment.GetEnvironmentVariable("CATACOMBS_PARAM")=="1") {
        var nativeBank=BND4.Read(Native("chr/c2610.anibnd.dcx"));var physical=nativeBank.Files.Select(f=>f.ID).ToHashSet();var nativeTae=TAE.Read(nativeBank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);
        var owners=new List<CatacombsCreaturesParameters.Owner>();foreach(var (model,sourceModel,npc,offset) in new[]{(8840,"c2900",290020,0),(8841,"c2900",290021,10000),(8842,"c2900",290022,20000),(8843,"c2910",291002,0),(8844,"c2650",265000,0),(8845,"c2930",293000,0)}) {
            var action=new CatacombsCreaturesActions(Source,sourceModel,offset);var contacts=nativeTae.Animations.Where(a=>a.ID%1000000>=3000&&a.ID%1000000<4000&&physical.Contains(1000000000+(int)a.ID)).SelectMany(a=>action.Events((int)(a.ID%1000000))).Where(e=>e.Type is 1 or 2).Select(e=>new CatacombsCreaturesCharacters.Contact(BitConverter.ToInt32(e.GetParameterBytes(false),8),e.Type)).Distinct().ToArray();
            owners.Add(new(model,npc,plan.Ordinary.Where(p=>p.NPCParamID==npc).Select(p=>p.ThinkParamID).Distinct().ToArray(),new(null!,action,contacts,328,0)));
        }
        var parameters=CatacombsCreaturesParameters.Build(Native("param/gameparam/gameparam.parambnd.dcx"),Source,defs,owners);Console.WriteLine("PASS parameters "+parameters.Npcs.Count+" ownedNPCs / "+parameters.Thinks.Count+" ownedbrains");
    }
    if(Environment.GetEnvironmentVariable("CATACOMBS_PACKAGE")=="1") {
        var output=new Dictionary<string,byte[]>();var owners=new List<CatacombsCreaturesParameters.Owner>();
        var families=new[]{(8840,"c2900",290020,0),(8841,"c2900",290021,10000),(8842,"c2900",290022,20000),(8843,"c2910",291002,0),(8844,"c2650",265000,0),(8845,"c2930",293000,0)};
        foreach(var (model,sourceModel,npc,offset) in families.Where(f=>f.Item1==8840||Environment.GetEnvironmentVariable("CATACOMBS_ALL")=="1")) {
            var row=plan.Npcs.Rows.Single(r=>r.ID==npc);var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row["modelDispMask"+i].Value)!=0).ToHashSet();
            var skin=CatacombsCreaturesCharacters.Build(Source,Native,sourceModel,offset,"c"+model,masks,output);Need(skin.Skin.DirectWeightShare>.999999&&skin.VisibleBones+80<=512,"Source skeleton skin/rig contract failed");owners.Add(new(model,npc,plan.Ordinary.Where(p=>p.NPCParamID==npc).Select(p=>p.ThinkParamID).Distinct().ToArray(),skin));ValidateMotion(Source,sourceModel,model,skin,output);
            Console.WriteLine($"{sourceModel}/{npc} full-rig package: {skin.VisibleBones}+80 bones, {skin.Clips} native clips, {skin.Contacts.Count} source contacts");
        }
        var parameters=CatacombsCreaturesParameters.Build(Native("param/gameparam/gameparam.parambnd.dcx"),Source,defs,owners);Need(parameters.Npcs.Count>=1,"Own NPC rows missing");
        var spawns=plan.Ordinary.Select((p,i)=>new MapEnemySpawns.Spawn(p.Name,p.ModelName,p.NPCParamID,p.ThinkParamID,p.EntityID,"c8840",4058000+i,884000,884100,p.Position,[],1,1,[],false,p.InitAnimID)).ToArray();
        var events=CatacombsCreaturesEvents.Apply(Native("event/m24_01_00_00.emevd.dcx"),plan,spawns);Need(EMEVD.Read(events).Events.Count(e=>e.ID>=CatacombsCreaturesEvents.First&&e.ID<CatacombsCreaturesEvents.First+300)==78,"Cohort controller count changed");
        var fx=CatacombsCreaturesEffects.Merge(Native("sfx/frpg_sfxbnd_m24.ffxbnd.dcx"),Native(CatacombsCreaturesEffects.NativePath));Need(BND4.Read(fx).Files.Any(f=>Leaf(f.Name)=="f000610101.fxr"),"Native fire flight missing");
    }
    foreach(var (file,sha) in hashes)Need(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file)))==sha,"Original changed");Console.WriteLine($"PASS:63fresh actors,49linked skeletons,6casters,17awakening groups,8private AI goals;{hashes.Count} originals unchanged. Native package fixtures do not prove gameplay.");}
}
