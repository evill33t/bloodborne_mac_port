// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
using System.Text.Json;

static class HollowCombatProfilesTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static string Leaf(string value)=>value.Replace('\\','/').Split('/').Last();
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}throw new Exception(message);}
    public static void Main(string[] args) {
        string source=args[0],native=args[1],definitions=args[2],output=args[3];var hashes=new Dictionary<string,byte[]>();
        byte[] Read(string root,string path){string file=Path.Combine(root,path);var bytes=File.ReadAllBytes(file);hashes.TryAdd(file,SHA256.HashData(bytes));return bytes;}
        var sourceBank=BND3.Read(Read(source,"param/GameParam/GameParam.parambnd.dcx"));var nativeBytes=Read(native,"param/gameparam/gameparam.parambnd.dcx");var nativeBank=BND4.Read(nativeBytes);
        PARAM Table(IEnumerable<BinderFile> bank,string name,string definition) {
            var table=PARAM.Read(bank.Single(f=>Leaf(f.Name)==name+".param").Bytes);table.ApplyParamdef(PARAMDEF.XmlDeserialize(definition));return table;
        }
        string bb=Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs"));
        var sourceRows=Table(sourceBank.Files,"NpcParam",Path.Combine(definitions,"NpcParam.xml"));
        var nativeRows=Table(nativeBank.Files,"NpcParam",Path.Combine(bb,"NpcParam.xml"));
        var nativeThinks=Table(nativeBank.Files,"NpcThinkParam",Path.Combine(bb,"NpcThinkParam.xml"));
        var effects=Table(nativeBank.Files,"SpEffectParam",Path.Combine(bb,"SpEffect.xml"));
        var opaque=PARAM.Read(nativeBank.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);
        var def=new PARAMDEF{ParamType="NPC_PARAM_ST",DataVersion=5,Unicode=true};def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=388});opaque.ApplyParamdef(def);
        int Value(PARAM.Row row,string key)=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName==key).Value);
        var ai=BND4.Read(Read(native,"script/"+HollowCombatProfiles.NativeAi+".luabnd.dcx"));var goals=LUAINFO.Read(ai.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)).Bytes);
        var placement=MSBB.Read(Read(native,"map/MapStudio/"+HollowCombatProfiles.NativeMap+".msb.dcx"));
        Check(placement.Models.Enemies.Count(m=>m.Name==HollowCombatProfiles.NativeModel)==1&&
            placement.Parts.Enemies.Any(p=>p.ModelName==HollowCombatProfiles.NativeModel&&p.Name=="c2630_0000"&&p.NPCParamID==263400&&p.ThinkParamID==263400&&p.EntityID==2410015),
            "Native hollow placement map lacks its model/validated actor donor");
        var placements=new List<object>();var counts=new Dictionary<HollowCombatProfiles.Kind,int>();var profiles=new List<HollowCombatProfiles.Profile>();
        foreach(string map in new[]{"m10_02_00_00","m10_01_00_00"}) {
            var msb=MSB1.Read(Read(source,"map/MapStudio/"+map+".msb"));
            foreach(var actor in msb.Parts.Enemies) {
                var row=sourceRows.Rows.SingleOrDefault(r=>r.ID==actor.NPCParamID);if(row==null)continue;
                var masks=Enumerable.Range(0,16).Where(i=>Value(row,"modelDispMask"+i)!=0).ToHashSet();
                var profile=HollowCombatProfiles.Select(actor,masks);if(profile==null)continue;profiles.Add(profile);
                counts[profile.Role]=counts.GetValueOrDefault(profile.Role)+1;
                var donor=nativeRows.Rows.Single(r=>r.ID==profile.NativeNpc);var brain=nativeThinks.Rows.Single(r=>r.ID==profile.NativeThink);
                Check(Value(brain,"logicId")==10000&&Value(brain,"battleGoalID")==profile.NativeBattleGoal,"Native role brain changed or uses unavailable logic");
                Check(goals.Goals.Any(g=>g.ID==profile.NativeBattleGoal)&&ai.Files.Any(f=>Leaf(f.Name)==profile.NativeBattleGoal.ToString("D6")+"_battle.lua"),"Native role lacks its actual battle script/registry");
                foreach(int effect in profile.RequiredEffects)Check(Enumerable.Range(0,8).Any(i=>Value(donor,"spEffectID"+i)==effect),"Role lost its animation-set/gun/turret switch");
                Check(Value(effects.Rows.Single(r=>r.ID==5400+profile.MotionBank),"animIdOffset")==profile.AnimationOffset,"Role animation bank is not the native effect contract");
                byte[] old=((byte[])opaque.Rows.Single(r=>r.ID==profile.NativeNpc).Cells.Single().Value).ToArray(),snapshot=old.ToArray();
                byte[] adapted=HollowCombatProfiles.Parameters(profile,old,row,definitions);
                Check(old.SequenceEqual(snapshot),"Role capsule conversion mutated native input bytes");
                foreach(var (key,at) in new[]{("hitHeight",16),("hitRadius",20),("hitYOffset",28)})
                    Check(BitConverter.ToSingle(adapted,at)==Convert.ToSingle(row.Cells.Single(c=>c.Def.InternalName==key).Value),"Source capsule field was omitted");
                Check(Value(donor,"normalChangeAnimChrId")==2630&&BitConverter.ToInt16(old,0x162)==2630&&BitConverter.ToInt16(adapted,0x162)==-1,
                    "Owned hollow animation still resolves the retail c2630 bank instead of its appended visible rig");
                for(int i=0;i<old.Length;i++)if(i is not(>=16 and <24 or >=28 and <32 or >=0x162 and <0x164))Check(old[i]==adapted[i],"Role adaptation changed a native combat byte outside source capsule/animation-owner fields");
                if(profile.Ranged)Check(profile.NativeBattleGoal==263030&&profile.MotionBank==2&&profile.NativeProjectileDummy==60&&profile.SourceProjectileDummy==(profile.Role==HollowCombatProfiles.Kind.Bow?30:20),"Native gun/source muzzle contract changed");
                placements.Add(new{map,actor.Name,actor.EntityID,profile});
            }
        }
        Check(profiles.Count==93&&counts[HollowCombatProfiles.Kind.Crossbow]==5&&counts[HollowCombatProfiles.Kind.CrossbowStationary]==4&&counts[HollowCombatProfiles.Kind.Bow]==1,"Source ordinary/bow population or crossbow role split changed");
        foreach(string model in profiles.Select(p=>p.SourceModel).Distinct()) {
            var bank=BND3.Read(Read(source,"chr/"+model+".chrbnd.dcx"));var geometry=FLVER2.Read(bank.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);
            Check(HollowCombatProfiles.NativeBones+geometry.Nodes.Count<=256,"Source/native combined rig exceeds encoded skin indices");
            foreach(var profile in profiles.Where(p=>p.SourceModel==model&&p.Ranged))Check(geometry.Dummies.Count(d=>d.ReferenceID==profile.SourceProjectileDummy)==1,"Source muzzle is missing or ambiguous");
        }
        var fixture=profiles.First();var sourceRow=sourceRows.Rows.Single(r=>r.ID==fixture.SourceNpc);byte[] payload=((byte[])opaque.Rows.Single(r=>r.ID==fixture.NativeNpc).Cells.Single().Value).ToArray();
        var example=new MSB1.Part.Enemy{Name="fixture",ModelName=fixture.SourceModel,NPCParamID=fixture.SourceNpc,ThinkParamID=fixture.SourceThink,InitAnimID=-1,CharaInitID=-1,TalkID=-1};
        Reject(()=>HollowCombatProfiles.Select(example,new HashSet<int>()),"Changed source masks were accepted");
        example.ThinkParamID=999999;Check(HollowCombatProfiles.Select(example,fixture.SourceMasks.ToHashSet())==null,"Unvalidated source brain was accepted");
        example.ThinkParamID=fixture.SourceThink;example.InitAnimID=9001;Check(HollowCombatProfiles.Select(example,fixture.SourceMasks.ToHashSet())==null,"Unadapted initial pose was accepted");
        Reject(()=>HollowCombatProfiles.Parameters(fixture,payload[..^1],sourceRow,definitions),"Malformed native role payload was accepted");
        var bad=payload.ToArray();BitConverter.GetBytes(26100).CopyTo(bad,0);Reject(()=>HollowCombatProfiles.Parameters(fixture,bad,sourceRow,definitions),"Another character's behavior prefix was accepted");
        bad=payload.ToArray();int effectIndex=Enumerable.Range(0,8).Single(i=>BitConverter.ToInt32(bad,72+i*4)==5400+fixture.MotionBank);BitConverter.GetBytes(-1).CopyTo(bad,72+effectIndex*4);
        Reject(()=>HollowCombatProfiles.Parameters(fixture,bad,sourceRow,definitions),"Missing native animation-bank switch was accepted");
        bad=payload.ToArray();BitConverter.GetBytes((short)2610).CopyTo(bad,0x162);
        Reject(()=>HollowCombatProfiles.Parameters(fixture,bad,sourceRow,definitions),"Unvalidated native animation owner was accepted");
        Check(Read(native,"param/gameparam/gameparam.parambnd.dcx").SequenceEqual(nativeBytes),"Native parameter bank changed");
        foreach(var (file,hash) in hashes)Check(SHA256.HashData(File.ReadAllBytes(file)).SequenceEqual(hash),"An original role asset was modified");
        Directory.CreateDirectory(output);File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{counts=counts.ToDictionary(p=>p.Key.ToString(),p=>p.Value),placements,originalsUnchanged=hashes.Count,limitations=HollowCombatProfiles.Limitations},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS: 92 existing hollows + one optional bow, five mobile/four stationary crossbows; native role scripts/effects, exact source capsules, owned animation banks, preserved combat payloads and {hashes.Count} unchanged original assets");
    }
}
