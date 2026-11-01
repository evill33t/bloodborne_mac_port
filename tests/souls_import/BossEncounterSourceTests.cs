// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

if(args.Length!=2)throw new ArgumentException("Pass source DSR root and DS1R definitions");
string root=Path.GetFullPath(args[0]),defs=Path.GetFullPath(args[1]);
var original=new Dictionary<string,byte[]>(StringComparer.Ordinal);var diskHashes=new Dictionary<string,string>(StringComparer.Ordinal);
string Hash(byte[] bytes)=>Convert.ToHexStringLower(SHA256.HashData(bytes));
byte[]? Read(string path) {
    if(!original.TryGetValue(path,out var bytes)){string p=Path.Combine(root,path);if(!File.Exists(p))return null;bytes=File.ReadAllBytes(p);original.Add(path,bytes);diskHashes.Add(path,Hash(bytes));}
    return bytes;
}
void Check(bool value,string message){if(!value)throw new InvalidDataException(message);}
int rejections=0;
void Reject(Action action,string message) {
    try{action();}catch(InvalidDataException){rejections++;return;}catch(NotSupportedException){rejections++;return;}catch(FileNotFoundException){rejections++;return;}
    throw new InvalidDataException("Unsupported source was accepted: "+message);
}
BossEncounterSource.Plan Load(Func<string,byte[]?>? read=null)=>BossEncounterSource.Load(read??Read,"DS1R",BossEncounterSource.SourceMap,defs);
var plan=Load();Check(plan.Encounters.Count==3&&plan.SourceEvents.Count==30,"exact source encounter/event count");
var t=plan.Encounters.Single(e=>e.Key=="taurus");var c=plan.Encounters.Single(e=>e.Key=="capra");
var g=plan.Encounters.Single(e=>e.Key=="gargoyles");
Check(t.Boss is {Model:"c2250",EntityID:1010700,NPCParamID:225000,ThinkParamID:225000}&&t.Boss.Position==new Vector3(-6.7f,26.98f,-110.4f),"Taurus actor contract");
Check(c.Boss is {Model:"c2240",EntityID:1010750,NPCParamID:224000,ThinkParamID:224001}&&c.Boss.Position==new Vector3(-81.43f,-43.57f,-1.49f),"Capra actor contract");
Check(t.Adds.Count==0&&c.Adds.Select(a=>a.EntityID).SequenceEqual(new[]{1010500,1010501})&&c.Adds.All(a=>a.NPCParamID==334003),"only the two primary source dogs are selected");
Check(c.SuppressedDuplicates.Select(a=>a.EntityID).SequenceEqual(new[]{1010510,1010511}),"alternate dog suppression is explicit");
Check(t.Entrance is {IdleAnimation:9001,EnterAnimation:9061,JumpDownMessage:700,JumpUpMessage:710}&&t.Entrance.Trigger.EntityID==1012701&&t.Entrance.TowerTop.EntityID==1012740&&t.Entrance.TowerBottom.EntityID==1012741,"authored Taurus intro and tower targets");
Check(t.EntryFog.EntityID==1011890&&t.ExitFogs.Single().EntityID==1011892&&c.EntryFog.EntityID==1011790&&c.ExitFogs.Count==0,"all three source fog objects");
Check(t.EntryFog.Entry?.EntityID==1012898&&t.EntryFog.RotateTarget?.EntityID==1012897&&t.StartRegion.EntityID==1012896,"Taurus outside/inside/start region identities");
Check(c.EntryFog.Entry?.EntityID==1012888&&c.EntryFog.RotateTarget?.EntityID==1012887&&c.StartRegion.EntityID==1012886,"Capra outside/inside/start region identities");
Check(t.ArenaRegion.Dimensions==new Vector3(79,29,49)&&c.ArenaRegion.Dimensions==new Vector3(33,12,13),"source arena dimensions preserve W,H,D axes");
Check(t.DefeatFlag==11010901&&c.DefeatFlag==11010902&&t.EnteredFlag==11015383&&c.EnteredFlag==11015373&&t.ActiveFlag==11015382&&c.ActiveFlag==11015372,"source defeat/entry/AI state remains distinct");
Check(t.Rewards.SoloSouls==3000&&c.Rewards.SoloSouls==6000&&t.Rewards.SourceNPCSouls==0&&c.Rewards.SourceNPCSouls==0,"GameArea boss reward, not NPC getSoul");
Check(t.Rewards.Immediate==null&&c.Rewards.Immediate is {ID:2510,CollectionFlag:50001510}&&c.Rewards.Immediate.Outcomes.Single()==new BossEncounterSource.LotOutcome(0x40000000,2014,1,100),"only Capra immediate key lot");
Check(plan.Encounters.Where(e=>e.Key!="gargoyles").All(e=>e.Rewards.DelaySeconds==5&&e.Rewards.Delayed.Select(l=>l.ID).SequenceEqual(new[]{9000,9030})&&e.Rewards.RandomDrops.Count==1),"delayed consumables remain separate from random weapon drops");
Check(t.EntryFog.BoxHalfExtents==new Vector3(1.2782525f,1.975f,.025f)&&c.EntryFog.BoxHalfExtents==new Vector3(1.725f,2.725f,.025f)&&t.EntryFog.BoxBodyTranslation==new Vector3(.021964788f,0,-.013287157f),"source fog HKX primitive dimensions and offsets");
Check(g.Boss is {Model:"c5350",EntityID:1010800,NPCParamID:535000,ThinkParamID:535000}&&g.SecondaryBoss is {Model:"c5350",EntityID:1010801,NPCParamID:535001,ThinkParamID:535001},"exact two Gargoyle combatants");
Check(g.SecondaryActivationRatio==.6f&&g.SecondaryIdleAnimation==7000&&g.SecondaryEnterAnimation==7001&&g.SecondaryStartRegion?.EntityID==1012650&&g.Adds.Count==0,"phase boss is not an unconditional ordinary add");
Check(g.Boss.DisplayMasks.SequenceEqual(new[]{0,1})&&g.SecondaryBoss!.DisplayMasks.SequenceEqual(new[]{2})&&g.ActorDisplayMasks[1010801].SequenceEqual(new[]{2}),"Gargoyle appearance masks remain per actor");
Check(g.DefeatFlag==3&&g.EnteredFlag==11015393&&g.ActiveFlag==11015392&&g.ProgressionFlag==11010000&&g.DefeatFlag!=11010700,"Gargoyle defeat does not ring the bell");
Check(g.CutsceneActor is {EntityID:1010802}&&g.CutsceneID==100110&&g.CutsceneTrigger is {Shape:"Cylinder",Radius:20}&&g.DetachedTailActors.Select(a=>a.EntityID).SequenceEqual(new[]{1010810,1010811}),"cutscene and tail dependencies stay separate from combatants");
Check(g.EntryFog.EntityID==1011990&&g.ExitFogs.Single().EntityID==1011992&&g.EntryFog.ConvexRadius==0&&g.EntryFog.BoxBodyTranslation==Vector3.Zero,"Gargoyle source fog primitives differ from Taurus");
Check(g.EntryFog.RotateTarget is {Shape:"Cylinder",Radius:.1f}&&g.StartRegion.EntityID==1012996&&g.ArenaRegion.Dimensions==new Vector3(30,20,70),"Gargoyle source cylindrical inside target and arena");
Check(g.Rewards.SoloSouls==10000&&g.Rewards.Immediate==null&&g.Rewards.Delayed.Single() is {ID:9020,CollectionFlag:-1}&&g.Rewards.Delayed[0].Outcomes.Single().ItemID==501&&g.Rewards.RandomDrops.Count==0,"Gargoyle clear reward is delayed TwinHumanities, not individual equipment");
Check(g.IndividualDeathDrops[1010800].Select(l=>l.ID).SequenceEqual(new[]{53500000,53500001,53500002})&&g.IndividualDeathDrops[1010801].Select(l=>l.ID).SequenceEqual(new[]{53500100,53500101})&&g.TailCutReward is {ID:53520000,CollectionFlag:51010970},"Gargoyle individual and tail reward routing");
// Independent typed TAG0 decode validates the hash-bound root-motion metadata.
void CheckMotion(BossEncounterSource.Actor actor,BossEncounterSource.Motion motion,int frames) {
    var bank=BND3.Read(Read("chr/"+actor.Model+".anibnd.dcx")!);
    var file=bank.Files.Single(f=>f.Name.Replace('\\','/').Split('/').Last()==motion.Clip);
    var tag=new SourceCollision.Tag(file.Bytes.ToArray());var animation=tag.Items.Single(i=>tag.Name(i.Type)=="hkaSplineCompressedAnimation");
    var frame=tag.Reference(tag.Nested(animation.Type,animation.At,"extractedMotion").At);var samples=tag.Array(frame,"referenceFrameSamples");
    int last=tag.Element(samples,samples.Count-1).At;float Float(int at)=>BitConverter.Int32BitsToSingle(tag.Int(at));
    Check(samples.Count==frames&&tag.Vector(tag.Element(samples,0).At)==Vector3.Zero&&tag.Vector(last)==motion.LocalDisplacement&&Float(last+12)==motion.YawDeltaRadians,"actual source motion endpoints and frame count");
    Check(Float(tag.Nested(animation.Type,animation.At,"duration").At)==motion.Duration&&tag.Vector(tag.Nested(frame.Type,frame.At,"up").At)==Vector3.UnitY&&tag.Vector(tag.Nested(frame.Type,frame.At,"forward").At)==Vector3.UnitZ,"source motion duration and coordinate basis");
    var world=actor.Position+Vector3.Transform(tag.Vector(last),Matrix4x4.CreateRotationY(actor.Rotation.Y*MathF.PI/180));
    Check(Vector3.Distance(world,motion.LandingPosition)<.00001f&&Math.Abs(motion.LandingYawDegrees-(actor.Rotation.Y+Float(last+12)*180/MathF.PI))<.00001f,"source motion composes with authored placement");
}
CheckMotion(t.Boss,t.Entrance!.RootMotion??throw new InvalidDataException("missing Taurus motion"),126);
CheckMotion(g.SecondaryBoss!,g.SecondaryMotion??throw new InvalidDataException("missing phase landing motion"),153);
// Verify the important behavioral claims independently of the helper digest.
const string mapPath="map/MapStudio/m10_01_00_00.msb",eventPath="event/m10_01_00_00.emevd.dcx",commonPath="event/common.emevd.dcx",paramPath="param/GameParam/GameParam.parambnd.dcx";
var events=EMEVD.Read(Read(eventPath)!);
var capraDeath=events.Events.Single(e=>e.ID==11010902);
Check(capraDeath.Instructions.Skip(8).Take(2).All(i=>i.Bank==2004&&i.ID==5)&&capraDeath.Instructions.Skip(8).Take(2).Select(i=>BitConverter.ToInt32(i.ArgData)).SequenceEqual(new[]{1010510,1010511}),"source fresh branch really suppresses alternate dogs");
Check(capraDeath.Instructions[16].Bank==2003&&capraDeath.Instructions[16].ID==2&&BitConverter.ToInt32(capraDeath.Instructions[16].ArgData)==11010902,"source sets the persistent defeat flag before BossDefeat");
Check(events.Events.Single(e=>e.ID==11015382).Instructions.Any(i=>i.Bank==2003&&i.ID==18&&BitConverter.ToInt32(i.ArgData,4)==9061),"source intro actually requests9061");
var phase=events.Events.Single(e=>e.ID==11015396);
Check(phase.Instructions[5].Bank==4&&phase.Instructions[5].ID==2&&BitConverter.ToInt32(phase.Instructions[5].ArgData,4)==1010800&&phase.Instructions[5].ArgData[8]==5&&BitConverter.ToSingle(phase.Instructions[5].ArgData,12)==.6f,"source phase actually waits for primaryHP<=60%");
var gargoyleDeath=events.Events.Single(e=>e.ID==11010001);
Check(gargoyleDeath.Instructions.Skip(5).Take(2).All(i=>i.Bank==4&&i.ID==0&&i.ArgData[0]==3)&&gargoyleDeath.Instructions.Skip(5).Take(2).Select(i=>BitConverter.ToInt32(i.ArgData,4)).SequenceEqual(new[]{1010800,1010801})&&BitConverter.ToInt32(gargoyleDeath.Instructions[8].ArgData)==3,"source requires both deaths before setting flag3");
var common=EMEVD.Read(Read(commonPath)!);var reward=common.Events.Single(e=>e.ID==970);
Check(reward.Instructions[0].Bank==1003&&reward.Instructions[0].ID==2&&reward.Instructions[5].Bank==1001&&reward.Instructions[5].ID==0&&BitConverter.ToSingle(reward.Instructions[5].ArgData)==5,"common970 skips defeated reloads and delays consumables5s");
Func<string,byte[]?> Override(string path,byte[] bytes)=>name=>name==path?bytes:Read(name);
Reject(()=>BossEncounterSource.Load(Read,"DS1",BossEncounterSource.SourceMap,defs),"original DS1 is not assumed identical");
Reject(()=>BossEncounterSource.Load(Read,"DS1R","m10_02_00_00",defs),"other source map");
Reject(()=>Load(name=>name==commonPath?null:Read(name)),"missing common rewards");
foreach(var contract in plan.SourceEvents) {
    var script=EMEVD.Read(Read(contract.Path)!);var e=script.Events.Single(e=>e.ID==contract.ID);e.Instructions[0].ArgData[0]^=1;
    Reject(()=>Load(Override(contract.Path,script.Write())),"mutated source event "+contract.ID);
}
var parameterScript=EMEVD.Read(Read(commonPath)!);parameterScript.Events.Single(e=>e.ID==970).Parameters[0].SourceStartByte+=4;
Reject(()=>Load(Override(commonPath,parameterScript.Write())),"source parameter patch");
var ctorScript=EMEVD.Read(Read(commonPath)!);var ctor=ctorScript.Events.Single(e=>e.ID==0);
var keyCall=ctor.Instructions.Single(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length==24&&BitConverter.ToInt32(i.ArgData,4)==970&&BitConverter.ToInt32(i.ArgData,8)==11010902);keyCall.ArgData[12]^=1;
Reject(()=>Load(Override(commonPath,ctorScript.Write())),"Capra reward constructor argument");
var branchScript=EMEVD.Read(Read(eventPath)!);branchScript.Events.Single(e=>e.ID==0).Instructions[93].ArgData[1]^=1;
Reject(()=>Load(Override(eventPath,branchScript.Write())),"source defeated/live branch selection");
var changed=MSB1.Read(Read(mapPath)!);changed.Parts.Enemies.Single(a=>a.EntityID==1010750).Position+=Vector3.UnitX;
Reject(()=>Load(Override(mapPath,changed.Write())),"changed boss placement");
changed=MSB1.Read(Read(mapPath)!);((MSB.Shape.Box)changed.Regions.GetEntries().Single(r=>r.EntityID==1012886).Shape).Depth+=1;
Reject(()=>Load(Override(mapPath,changed.Write())),"changed encounter region");
changed=MSB1.Read(Read(mapPath)!);changed.Parts.Objects.Single(a=>a.EntityID==1011890).Rotation+=Vector3.UnitY;
Reject(()=>Load(Override(mapPath,changed.Write())),"changed fog placement");
var fog=Read("obj/o1405.objbnd.dcx")!.ToArray();fog[^1]^=1;Reject(()=>Load(Override("obj/o1405.objbnd.dcx",fog)),"changed fog shape binder");
var gargoyleMaskGame=BND3.Read(Read(paramPath)!);var maskFile=gargoyleMaskGame.Files.Single(f=>f.Name.Replace('\\','/').Split('/').Last()=="NpcParam.param");var maskRows=PARAM.Read(maskFile.Bytes);maskRows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,"NpcParam.xml")));maskRows.Rows.Single(r=>r.ID==535001).Cells.Single(c=>c.Def.InternalName=="modelDispMask2").Value=(byte)0;maskFile.Bytes=maskRows.Write();
Reject(()=>Load(Override(paramPath,gargoyleMaskGame.Write())),"changed secondary Gargoyle appearance");
foreach(var (model,motion) in new[]{(t.Boss.Model,t.Entrance!.RootMotion!),(g.SecondaryBoss!.Model,g.SecondaryMotion!)}) {
    string path="chr/"+model+".anibnd.dcx";var bank=BND3.Read(Read(path)!);var file=bank.Files.Single(f=>f.Name.Replace('\\','/').Split('/').Last()==motion.Clip);var bytes=file.Bytes.ToArray();bytes[^1]^=1;file.Bytes=bytes;
    Reject(()=>Load(Override(path,bank.Write())),"changed source extracted-motion clip");
}
var game=BND3.Read(Read(paramPath)!);var areaFile=game.Files.Single(f=>f.Name.Replace('\\','/').Split('/').Last()=="GameAreaParam.param");
var areas=PARAM.Read(areaFile.Bytes);areas.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,"GameAreaParam.xml")));areas.Rows.Single(r=>r.ID==1010750).Cells.Single(c=>c.Def.InternalName=="bonusSoul_single").Value=1u;areaFile.Bytes=areas.Write();
Reject(()=>Load(Override(paramPath,game.Write())),"changed boss reward");
// Unrelated source events can change without invalidating these contracts.
var unrelated=EMEVD.Read(Read(eventPath)!);unrelated.Events.Add(new EMEVD.Event(11999990));Check(Load(Override(eventPath,unrelated.Write())).Encounters.Count==3,"contract scope excludes unrelated events");
foreach(var (path,hash) in diskHashes){Check(Hash(original[path])==hash,"source buffer was mutated: "+path);Check(Hash(File.ReadAllBytes(Path.Combine(root,path)))==hash,"source asset was written: "+path);}
string output="out/map-editor/boss-encounters/source";Directory.CreateDirectory(output);
string serialized=JsonSerializer.Serialize(plan,new JsonSerializerOptions{WriteIndented=true,IncludeFields=true});
using(var json=JsonDocument.Parse(serialized)) {
    var encounter=json.RootElement.GetProperty("Encounters").EnumerateArray().Single(e=>e.GetProperty("Key").GetString()=="gargoyles");
    Check(encounter.GetProperty("SecondaryBoss").GetProperty("EntityID").GetInt32()==1010801&&encounter.GetProperty("SecondaryActivationRatio").GetSingle()==.6f&&encounter.GetProperty("SecondaryMotion").GetProperty("Duration").GetSingle()>5,"serialized plan retains phase and motion contracts");
}
File.WriteAllText(Path.Combine(output,"plan.json"),serialized);
Console.WriteLine($"PASS:3encounters,5fogwalls,2primarydogs,2suppressedduplicates,2phasecombatants,30sourceeventcontracts,2decodedrootmotions,{rejections}unsupportedmutations rejected,{diskHashes.Count}sourceassets preserved");

// SourceCollision's unused map-mesh reader needs only this value type. This
// fixture does not compile or execute a native collision writer.
internal static class MapCollision {internal record Triangle(Vector3 A,Vector3 B,Vector3 C,uint? Material=null);}
