// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
static class MapStandingVariantTests {
    static void Check(bool value,string reason){if(!value)throw new Exception(reason);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static bool Constant(byte[] bytes,string text) {
        var value=Encoding.ASCII.GetBytes(text+'\0');int at=bytes.AsSpan().IndexOf(value);
        return at>=9&&bytes[at-9]==4&&BitConverter.ToUInt64(bytes,at-8)==(ulong)value.Length;
    }
    public static void Main(string[] args) {
        string source=args[0],native=args[1],defs=args[2];var hashes=new Dictionary<string,string>();
        byte[] Read(string path){var data=File.ReadAllBytes(path);hashes.TryAdd(path,Convert.ToHexString(SHA256.HashData(data)));return data;}
        byte[]? Source(string p)=>File.Exists(Path.Combine(source,p))?Read(Path.Combine(source,p)):null;
        byte[]? Native(string p)=>File.Exists(Path.Combine(native,p))?Read(Path.Combine(native,p)):null;
        var map=MSB1.Read(Source("map/MapStudio/m10_01_00_00.msb")!);
        int[] candidates=[250052,250053,254013,254014,255043,255044,256004];
        var actors=map.Parts.Enemies.Where(p=>candidates.Contains(p.NPCParamID)&&p.InitAnimID==-1&&p.CharaInitID<0&&p.TalkID<=0).ToArray();Check(actors.Length==10,"Standing-variant audit population changed");
        var sourceBank=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx")!);
        PARAM Table(string name){var rows=PARAM.Read(sourceBank.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);rows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,name+".xml")));return rows;}
        var npcs=Table("NpcParam");var thinks=Table("NpcThinkParam");int Cell(PARAM.Row row,string name)=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName==name).Value);
        var phantom=actors.Where(a=>a.NPCParamID!=250052).ToArray();Check(phantom.Length==9,"Conditional phantom population changed");
        var script=EMEVD.Read(Source("event/m10_01_00_00.emevd.dcx")!);var activation=script.Events.Single(e=>e.ID==11015070);var cleanup=script.Events.Single(e=>e.ID==11015071);
        var constructor=script.Events.Single(e=>e.ID==0);
        foreach(int id in new[]{11015070,11015071})Check(constructor.Instructions.Any(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length>=8&&BitConverter.ToInt32(i.ArgData,4)==id),"Source phantom controller is not initialized");
        Check(activation.Instructions.Any(i=>i.Bank==3&&i.ID==0&&i.ArgData.Length==8&&BitConverter.ToInt32(i.ArgData,4)==11010050)&&activation.Instructions.Any(i=>i.Bank==1003&&i.ID==2&&i.ArgData.Length==8&&BitConverter.ToInt32(i.ArgData,4)==735),"Source phantom activation guards changed");
        foreach(var actor in phantom) {
            var row=npcs.Rows.Single(r=>r.ID==actor.NPCParamID);Check(Cell(row,"drawType")==2&&Cell(row,"spEffectID6")==7100&&Cell(row,"disableInitializeDead")==1,"Conditional phantom parameter contract changed");
            int disabled=activation.Instructions.FindIndex(i=>i.Bank==2004&&i.ID==5&&i.ArgData.Length==8&&BitConverter.ToInt32(i.ArgData,0)==actor.EntityID&&i.ArgData[4]==0);
            int enabled=activation.Instructions.FindIndex(i=>i.Bank==2004&&i.ID==5&&i.ArgData.Length==8&&BitConverter.ToInt32(i.ArgData,0)==actor.EntityID&&i.ArgData[4]==1);
            Check(disabled>=0&&enabled>disabled&&activation.Instructions.Skip(disabled+1).Take(enabled-disabled-1).Any(i=>i.Bank==3&&i.ID==0),"A phantom can be spawned without its source activation guard");
            Check(cleanup.Instructions.Any(i=>i.Bank==2004&&i.ID==4&&i.ArgData.Length==8&&BitConverter.ToInt32(i.ArgData,0)==actor.EntityID),"Source phantom cleanup contract changed");
        }
        var archer=actors.Single(a=>a.NPCParamID==250052);var archerRow=npcs.Rows.Single(r=>r.ID==250052);var archerThink=thinks.Rows.Single(r=>r.ID==archer.ThinkParamID);
        Check(archer.Name=="c2500_0051"&&archer.EntityID==-1&&archer.ThinkParamID==250020&&Cell(archerRow,"animIdOffset")==20000&&Cell(archerRow,"aiThinkId")==250005&&Cell(archerThink,"battleGoalID")==250005,"Source standing archer contract changed");
        Check(Enumerable.Range(0,16).Where(i=>Cell(archerRow,"modelDispMask"+i)!=0).SequenceEqual(new[]{2}),"Archer equipment mask changed");
        var ai=BND3.Read(Source("script/m10_01_00_00.luabnd.dcx")!);var bow=ai.Files.Single(f=>Leaf(f.Name)=="250005_battle.lua").Bytes.ToArray();
        Check(Constant(bow,"GOAL_Husi_Bow250005_Battle")&&Constant(bow,"Shoot_Act")&&Constant(bow,"GOAL_COMMON_LeaveTarget"),"Source archer is no longer a ranged shooting controller");
        var skinBank=BND3.Read(Source("chr/c2500.chrbnd.dcx")!);var skin=FLVER2.Read(skinBank.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);
        var bows=skin.Meshes.Where(m=>skin.Materials[m.MaterialIndex].Name=="#02#").ToArray();var weighted=bows.SelectMany(m=>m.Vertices.SelectMany(v=>Enumerable.Range(0,4).Where(i=>v.BoneWeights[i]>0).Select(i=>skin.Nodes[SkinRetarget.Bone(m,v.BoneIndices[i])].Name))).ToHashSet(StringComparer.Ordinal);
        Check(bows.Length==1&&new[]{"bow_01","bow_string","bpw_00"}.All(weighted.Contains),"Source bow rig evidence changed");
        var identities=new List<object>();var newSkips=new List<MapEnemySpawns.Omitted>();byte[] composed=Native("param/gameparam/gameparam.parambnd.dcx")!;int ordinary=0;
        foreach(string id in new[]{"m10_02_00_00","m10_01_00_00"}) {
            var sourceMap=MSB1.Read(Source("map/MapStudio/"+id+".msb")!);var destination=MSBB.Read(Native("map/MapStudio/m24_01_00_01.msb.dcx")!);MapReplacement.ParkActors(destination);
            var offset=new Vector3(-143.8f,20.07f,164.63f);
            // This identity fixture needs solid residency for the independently
            // adapted boss actors; actual collision coverage has separate tests.
            var floors=sourceMap.Parts.Enemies.Where(p=>BossEnemyCharacters.Select(p,id)!=null).Select(p=>p.Position+offset-new Vector3(0,.1f,0))
                .Select(p=>new MapCollision.Triangle(p+new Vector3(-2,0,-2),p+new Vector3(2,0,-2),p+new Vector3(0,0,2))).ToArray();
            var slots=destination.Parts.Collisions.DistinctBy(c=>c.ModelName).ToArray();var chunks=slots.Select(_=>new MapCollisionBindings.Chunk(sourceMap.Parts.Collisions.First().Name,floors,[])).ToArray();
            var imported=MapEnemySpawns.Bake(Source,sourceMap,id,defs,native,destination,offset,chunks,slots,composed,readNative:Native);composed=imported.Files["param/gameparam/gameparam.parambnd.dcx"];
            Check(imported.Spawns.Where(p=>p.SourceModel=="c2560").All(p=>p.SourceClothPose is{Bodies:57,Constraints:119} report&&report.ChangedVertices>0),"Source cape appearance report is missing");
            if(id=="m10_01_00_00") {newSkips.AddRange(imported.Skipped.Where(s=>candidates.Contains(s.SourceNPCParamID)));Check(imported.Spawns.All(s=>!candidates.Contains(s.SourceNPCParamID)),"Conditional phantoms or unsupported bow archer were spawned as melee enemies");}
            foreach(var a in imported.Spawns.Where(s=>s.NativeAdapter==null&&s.SourceModel is not("c2900" or "c2910"))) {ordinary++;identities.Add(new{map=id,a.SourceName,a.SourceModel,a.SourceNPCParamID,a.TargetModel,a.EntityID,NPCParamID=a.NPCParamID/100*100});}
        }
        Check(ordinary==92&&newSkips.Count==10,"Existing cohorts or unsupported standing population changed");
        Check(newSkips.Where(s=>s.SourceNPCParamID!=250052).All(s=>s.Reason.Contains("Gravelord",StringComparison.Ordinal))&&newSkips.Single(s=>s.SourceNPCParamID==250052).Reason.Contains("bow",StringComparison.OrdinalIgnoreCase),"Standing omissions hide their actual source contracts");
        string json=JsonSerializer.Serialize(identities),identityHash=Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(json)));
        // Derived from the preserved old92-identities.json baseline. Native
        // Think/controller roles now vary; the source/model/entity and owned
        // hundred-row NPC allocation stay stable across those role variants.
        const string expected="DED63A91EEE0F163382FF1F2A9215317DAD9C597E5BB20174B5B1540ADC75454";
        Console.WriteLine("Existing92IdentitySHA256="+identityHash);
        Check(identityHash==expected,"Existing 92 hollow source/model/entity/NPC-block identities changed");
        foreach(var (path,hash) in hashes)Check(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)))==hash,"Original asset changed: "+path);
        Directory.CreateDirectory(args[3]);File.WriteAllText(Path.Combine(args[3],"standing-variant-validation.json"),JsonSerializer.Serialize(new{status="PASS",ordinary,identityHash,identities,conditional=phantom.Select(a=>new{a.Name,a.NPCParamID,a.EntityID}),archer=new{archer.Name,archer.NPCParamID,archer.ThinkParamID},newSkips,hashes},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS: 9 conditional phantoms and 1 ranged bow variant remain excluded; existing {ordinary} hollow identities, {hashes.Count} original hashes preserved");
    }
}
