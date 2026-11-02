// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
using System.Text.Json;
using K=HollowCombatProfiles.Kind;

if(args.Length!=2)throw new ArgumentException("Supply DS1R and Bloodborne roots");
string source=Path.GetFullPath(args[0]),native=Path.GetFullPath(args[1]);
var hashes=new Dictionary<string,string>();
byte[] Read(string path){var b=File.ReadAllBytes(path);hashes.TryAdd(path,Convert.ToHexStringLower(SHA256.HashData(b)));return b;}
byte[]? SourceRead(string p)=>File.Exists(Path.Combine(source,p))?Read(Path.Combine(source,p)):null;
void Check(bool b,string why){if(!b)throw new Exception(why);}
void Reject(System.Action action,string why){try{action();}catch(Exception e)when(e is InvalidDataException or ArgumentException or NotSupportedException){return;}throw new Exception("Accepted "+why);}
string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
int Jump(TAE.Event e)=>e.Type==0?BitConverter.ToInt32(e.GetParameterBytes(false)):-1;
bool Gun(TAE.Event e,K role){if(role is not(K.Crossbow or K.CrossbowStationary or K.Bow)||e.Type!=96)return false;var b=e.GetParameterBytes(false);return (BitConverter.ToInt32(b),BitConverter.ToInt32(b,4)) is (1033000,6) or (610205,80);}
string Group(TAE.Event e){var g=e.Group;if(g==null)return "none";var d=g.GroupData;return $"{g.GroupType}:{d.DataType}:{d.CutsceneEntityType}:{d.CutsceneEntityIDPart1}:{d.CutsceneEntityIDPart2}:{d.Area}:{d.Block}";}
string Event(TAE.Event e,bool raw=false){var bytes=e.GetParameterBytes(false);int length=bytes.Length;if(!raw)while(length>0&&bytes[length-1]==0)--length;return $"{e.Type}:{e.StartTime:R}:{e.EndTime:R}:{e.Unk04}:{Convert.ToHexString(bytes.AsSpan(0,length))}:{Group(e)}";}
float Duration(byte[] bytes) {
    // Independent retail packfile virtual-fixup reader; only the duration
    // field is needed, never a sampled or implementation-derived golden.
    Check(BitConverter.ToUInt32(bytes)==0x57e0e057&&BitConverter.ToUInt32(bytes,4)==0x10c0c010,"not a native packfile");
    int section=64+BitConverter.ToUInt16(bytes,62),classes=BitConverter.ToInt32(bytes,section+20),header=section+128,data=BitConverter.ToInt32(bytes,header+20),virtuals=BitConverter.ToInt32(bytes,header+32),end=BitConverter.ToInt32(bytes,header+36);
    for(int i=virtuals;i+12<=end;i+=12){int obj=BitConverter.ToInt32(bytes,data+i),part=BitConverter.ToInt32(bytes,data+i+4),name=BitConverter.ToInt32(bytes,data+i+8);if(obj<0)continue;Check(part==0,"unexpected class section");int at=classes+name;string type=System.Text.Encoding.ASCII.GetString(bytes,at,Array.IndexOf(bytes,(byte)0,at)-at);if(type=="hkaSplineCompressedAnimation")return BitConverter.ToSingle(bytes,data+obj+16);}
    throw new Exception("native spline object missing");
}
var raw=Read(Path.Combine(native,"chr/c2630.anibnd.dcx"));var bank=BND4.Read(raw);var taeFile=bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));
var original=TAE.Read(taeFile.Bytes);var canonical=TAE.Read(original.Write()); // Reader exposes alignment padding as parameter bytes on write/read.
var physical=bank.Files.Where(f=>f.ID>=1000000000).ToDictionary(f=>f.ID-1000000000,f=>Duration(f.Bytes.ToArray()));
var roles=new (string Model,K Role)[]{("c2500",K.Sword),("c2500",K.Torch),("c2500",K.Bow),("c2540",K.SwordShield),("c2540",K.Axe),("c2540",K.Firebomb),("c2550",K.SpearShield),("c2550",K.SwordShield),("c2550",K.Crossbow),("c2550",K.CrossbowStationary),("c2560",K.SwordShield),("c2560",K.RapierShield)};
var reports=new List<object>();
foreach(var role in roles) {
    var plan=HollowCombatMotion.Create(SourceRead,role.Model,role.Role,bank);
    Check(plan.MotionSourceIDs.Count==567&&plan.Mappings.Count==657&&plan.SharedNativeActions.Count==31&&physical.Keys.ToHashSet().SetEquals(plan.MotionSourceIDs.Keys),"incomplete native motion coverage");
    var sourceBank=BND3.Read(SourceRead("chr/"+role.Model+".anibnd.dcx")!);var sourceTae=TAE.Read(sourceBank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);var sourceActions=sourceTae.Animations.ToDictionary(a=>checked((int)a.ID));
    TAE.Animation SourceEvents(int id,HashSet<int>? seen=null){seen??=[];Check(seen.Add(id),"test source import cycle");var a=sourceActions[id];return a.MiniHeader is TAE.Animation.AnimMiniHeader.ImportOtherAnim i?SourceEvents(i.ImportFromAnimID,seen):a;}
    var durations=plan.Mappings.ToDictionary(m=>m.NativeID,m=>physical[plan.NativeMotionID(m.NativeID)]);var current=TAE.Read(taeFile.Bytes);
    var result=HollowCombatMotion.Apply(current,plan,durations);
    Check(current.Animations.All(a=>a.Events.All(e=>e.Group==null||a.EventGroups.Contains(e.Group))),"event references a foreign animation group before write");
    foreach(var mapping in plan.Mappings){var before=original.Animations.Single(a=>a.ID==mapping.NativeID);var after=current.Animations.Single(a=>a.ID==mapping.NativeID);var kept=before.Events.Where(e=>(!mapping.Combat||(e.Type is not(1 or 2)&&Jump(e) is not(23 or 78 or 79 or 86)))&&!Gun(e,role.Role)).Select(e=>Event(e,true)).ToArray();Check(kept.SequenceEqual(after.Events.Take(kept.Length).Select(e=>Event(e,true))),"unrelated native raw parameters changed before serialization");}
    // SoulsFormats includes alignment bytes in unknown event payloads; their
    // zero padding can grow after the event/time tables change. Raw bytes were
    // checked above; readback checks semantic bytes, allowing only zero tails.
    var readback=TAE.Read(current.Write());
    Check(readback.Animations.All(a=>a.Events.All(e=>e.Group==null||a.EventGroups.Contains(e.Group))),"event references a foreign animation group after readback");
    Check(result.Actions.Count==657&&readback.Animations.Count==original.Animations.Count,"native TAE action population changed");
    foreach(int id in plan.SharedNativeActions)Check(canonical.Animations.Single(a=>a.ID==id).Events.Select(e=>Event(e)).SequenceEqual(readback.Animations.Single(a=>a.ID==id).Events.Select(e=>Event(e))),"shared/additive native action changed");
    int hits=0,shots=0,gates=0;
    foreach(var mapping in plan.Mappings) {
        var before=canonical.Animations.Single(a=>a.ID==mapping.NativeID);var after=readback.Animations.Single(a=>a.ID==mapping.NativeID);
        Check(before.AnimFileName==after.AnimFileName&&before.MiniHeader.Type==after.MiniHeader.Type,"native animation reference changed");
        var retained=before.Events.Where(e=>(!mapping.Combat||(e.Type is not(1 or 2)&&Jump(e) is not(23 or 78 or 79 or 86)))&&!Gun(e,role.Role)).Select(e=>Event(e));
        var actualRetained=after.Events.Take(retained.Count()).Select(e=>Event(e)).ToArray();var expectedRetained=retained.ToArray();
        int mismatch=Enumerable.Range(0,expectedRetained.Length).FirstOrDefault(i=>expectedRetained[i]!=actualRetained[i],-1);
        Check(mismatch<0,"unrelated native sound/effect/physics event changed: "+role+"/"+mapping.NativeID+" #"+mismatch+" expected "+(mismatch<0?"":expectedRetained[mismatch])+" actual "+(mismatch<0?"":actualRetained[mismatch]));
        if(!mapping.Combat)continue;
        var from=SourceEvents(mapping.SourceID);var expected=from.Events.Where(e=>e.Type is 1 or 2||Jump(e) is 23 or 78 or 79 or 86).ToArray();
        var actual=after.Events.Skip(retained.Count()).ToArray();Check(actual.Length==expected.Length,"source event multiplicity lost");
        // Production groups hit, shot, then continuation events; compare each
        // type independently with raw source values and independent durations.
        foreach(int type in new[]{1,2,0}) {
            var se=expected.Where(e=>e.Type==type).ToArray();var ae=actual.Where(e=>e.Type==type).ToArray();Check(se.Length==ae.Length,"type population mismatch");
            for(int i=0;i<se.Length;i++) {
                float scale=durations[mapping.NativeID]/plan.Action(mapping.NativeID).Clip.Duration;
                Check(Math.Abs(ae[i].StartTime-se[i].StartTime*scale)<.0001f&&Math.Abs(ae[i].EndTime-se[i].EndTime*scale)<.0001f,"hit/shot does not follow visible normalized source pose");
                if(type==0){Check(ae[i].GetParameterBytes(false).AsSpan(0,12).SequenceEqual(se[i].GetParameterBytes(false).AsSpan(0,12)),"source continuation/cancel arguments lost");Check(Group(ae[i])==Group(original.Animations.Single(a=>a.ID==mapping.NativeID).Events.First(e=>e.Type==0)),"native continuation group changed/lost");}
                else {int carrier=type==1?mapping.HitCarrier:mapping.BulletCarrier;if(carrier<0)carrier=mapping.NativeID;var prototype=original.Animations.Single(a=>a.ID==carrier).Events.First(e=>e.Type==type);Check(ae[i].GetParameterBytes(false).AsSpan().SequenceEqual(prototype.GetParameterBytes(false)),"native behavior/projectile carrier changed");Check(Group(ae[i])==Group(prototype),"native imported carrier group changed/lost");}
            }
        }
        hits+=expected.Count(e=>e.Type==1);shots+=expected.Count(e=>e.Type==2);gates+=expected.Count(e=>e.Type==0);
    }
    foreach(var reaction in plan.Mappings.Where(m=>m.NativeID%1000000 is >=2600 and <=2623 or >=2900 and <=2903)) {
        Check(reaction.SourceID%10000!=2020,"Normal interrupt/visceral wait uses a knockdown");
        var c=plan.Action(reaction.NativeID).Clip;var w=c.Sample(c.Duration*.5f).ToArray();for(int i=0;i<w.Length;i++)if(c.Bones[i].Parent>=0)w[i]*=w[c.Bones[i].Parent];
        Check(w[c.Bones.Select((b,i)=>(b.Name,i)).Single(b=>b.Name=="Head").i].Translation.Y>.7f,"Normal hit reaction lies on the floor");
    }
    if(role.Role is K.Crossbow or K.CrossbowStationary or K.Bow) {
        Check(plan.SourceID(2003031)==23000&&plan.SourceID(2003032)==23001,"ranged action lost full source bank");
        Check(plan.Action(2003031).Filename=="a02_3000.hkx","ranged motion resolved to melee bank");
        var shot=readback.Animations.Single(a=>a.ID==2003031).Events.Single(e=>e.Type==2);Check(BitConverter.ToInt32(shot.GetParameterBytes(false))==60,"native muzzle dummy changed");
    }
    if(role.Role==K.Axe)Check(plan.SourceID(2003023)==13000&&plan.Action(2003023).Filename=="a01_3000.hkx","axe resolved to sword bank");
    var pristine=TAE.Read(taeFile.Bytes);var baseline=pristine.Write();var missing=new Dictionary<int,float>(durations);missing.Remove(plan.Mappings.Last().NativeID);
    Reject(()=>HollowCombatMotion.Apply(pristine,plan,missing),"missing duration");Check(baseline.AsSpan().SequenceEqual(pristine.Write()),"validation failure partially mutated native events");
    reports.Add(new{role.Model,Role=role.Role.ToString(),Physical=plan.MotionSourceIDs.Count,Actions=plan.Mappings.Count,ReferenceFallbacks=plan.ReferenceFallbackIDs.Count,Hits=hits,Shots=shots,Gates=gates});
    Console.WriteLine($"PASS {role.Model}/{role.Role}:567motions,657local/31sharedactions,{hits}hits/{shots}shots/{gates}gates");
}
var sourceRaw=SourceRead("chr/c2500.anibnd.dcx")!;var duplicate=BND3.Read(sourceRaw);duplicate.Files.Add(duplicate.Files.First(f=>Leaf(f.Name)=="a00_3000.hkx").Clone());
Reject(()=>HollowCombatMotion.Load(p=>duplicate.Write(),"c2500","bad",[new(3000,3000)]),"ambiguous source file");
var cycle=BND3.Read(sourceRaw);var file=cycle.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));var t=TAE.Read(file.Bytes);var action=t.Animations.Single(a=>a.ID==3000);action.MiniHeader=new TAE.Animation.AnimMiniHeader.ImportOtherAnim{ImportFromAnimID=3000};file.Bytes=t.Write();
Reject(()=>HollowCombatMotion.Load(p=>cycle.Write(),"c2500","cycle",[new(3000,3000)]),"cyclic source import");
var badBank=BND4.Read(raw);badBank.Files.Remove(badBank.Files.First(f=>f.ID>=1000000000));Reject(()=>HollowCombatMotion.Create(SourceRead,"c2500",K.Sword,badBank),"changed native motion population");
foreach(var(path,hash) in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==hash,"original asset changed");
Directory.CreateDirectory("out/map-editor/hollow-combat-motion-tests");File.WriteAllText("out/map-editor/hollow-combat-motion-tests/report.json",JsonSerializer.Serialize(new{Roles=reports,InputsPreserved=hashes.Count,Limit="Installed-asset serialization/normalized-event fixture; native gameplay remains separate."},new JsonSerializerOptions{WriteIndented=true}));
Console.WriteLine("PASS source aliases/ambiguity/atomicity, native payload and source-asset preservation");
