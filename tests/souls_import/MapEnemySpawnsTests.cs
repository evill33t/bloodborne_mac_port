// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

static class MapEnemySpawnsTests {
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    public static void Main(string[] args) {
        string root=args[0],native=args[1],defs=args[2];var originals=new Dictionary<string,byte[]>();
        byte[]? Read(string path){string p=Path.Combine(root,path);if(!File.Exists(p))return null;var b=File.ReadAllBytes(p);originals.TryAdd(p,SHA256.HashData(b));return b;}
        byte[] baseline=File.ReadAllBytes(Path.Combine(native,"param/gameparam/gameparam.parambnd.dcx")),composed=baseline;
        var allModels=new HashSet<string>();int total=0;
        foreach(var id in new[]{"m10_02_00_00","m10_01_00_00"}) {
            var source=MSB1.Read(Read("map/MapStudio/"+id+".msb")!);
            var map=MSBB.Read(File.ReadAllBytes(Path.Combine(native,"map/MapStudio/m24_01_00_01.msb.dcx")));MapReplacement.ParkActors(map);
            var slots=map.Parts.Collisions.DistinctBy(p=>p.ModelName).ToArray();var chunks=slots.Select(p=>new MapCollisionBindings.Chunk(source.Parts.Collisions.First().Name,[],[])).ToArray();
            var offset=new Vector3(-143.8f,20.07f,164.63f);
            var result=MapEnemySpawns.Bake(Read,source,id,defs,native,map,offset,chunks,slots,composed);
            Check(result.Spawns.Count==(id=="m10_02_00_00"?6:86),"Ordinary hollow selection changed or conditional populations were included");
            Check(result.Skipped.Count+result.Spawns.Count==source.Parts.Enemies.Count,"Source actors were not completely accounted for");
            Check(result.Spawns.All(p=>source.Parts.Enemies.Single(s=>s.Name==p.SourceName).InitAnimID==-1),"A hanging/special-pose actor was spawned in a standing pose");
            var old=BND4.Read(composed);composed=result.Files["param/gameparam/gameparam.parambnd.dcx"];var game=BND4.Read(composed);
            foreach(var f in old.Files.Where(f=>Leaf(f.Name)!="NpcParam.param"))Check(game.Files.Single(n=>n.ID==f.ID).Bytes.Span.SequenceEqual(f.Bytes.Span),"Enemy export overwrote an unrelated parameter entry");
            var oldNpcs=MapCharacters.NativeRows(old.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);
            var npcs=MapCharacters.NativeRows(game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);
            for(int i=0;i<oldNpcs.Rows.Count;i++)Check(npcs.Rows[i].ID==oldNpcs.Rows[i].ID&&MapCharacters.Payload(npcs.Rows[i]).SequenceEqual(MapCharacters.Payload(oldNpcs.Rows[i])),"Existing composed NPC row changed");
            foreach(var group in result.Spawns.GroupBy(p=>p.TargetModel)) {
                Check(allModels.Add(group.Key),"Two maps reuse an imported character namespace");
                var bnd=BND4.Read(result.Files["chr/"+group.Key+".chrbnd.dcx"]);var flver=FLVER2.Read(bnd.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);
                var bank=TPF.Read(bnd.Files.Single(f=>TPF.Is(f.Bytes.ToArray())).Bytes);
                Check(bank.Textures.All(t=>t.Header.Unk2==0xD),"PS4 texture lacks native tiling metadata");
                Check(flver.Meshes.Count>0&&group.All(p=>p.DirectWeightShare>.6&&p.Bones.Where(b=>new[]{"head","pelvis","lfoot","rfoot","lhand","rhand"}.Contains(new string(b.Source.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray()))).All(b=>b.Direct)),"Hollow skin was substituted or poorly matched");
                var param=npcs.Rows.Single(r=>r.ID==group.First().NPCParamID);var expected=MapCharacters.Payload(oldNpcs.Rows.Single(r=>r.ID==263400));foreach(int i in new[]{0x146,0x147,0x14A,0x14B})expected[i]=255;
                Check(MapCharacters.Payload(param).SequenceEqual(expected),"Native combat/AI donor changed beyond display masks");
                foreach(var spawn in group) {
                    var actor=map.Parts.Enemies.Single(p=>p.EntityID==spawn.EntityID);var original=source.Parts.Enemies.Single(p=>p.Name==spawn.SourceName);
                    Check(actor.Position==original.Position+offset&&actor.Rotation==original.Rotation&&Vector3.Distance(actor.Scale,original.Scale*spawn.ProportionScale)<.0001f,"Enemy source transform changed");
                    Check(actor.ThinkParamID==263400&&actor.TalkID==-1&&actor.InitAnimID==-1&&actor.CharaInitID==-1,"Enemy retained incompatible source behavior IDs");
                    Check(spawn.DisplayMasks.Length>0,"Source equipment masks were omitted");
                }
            }
            var back=MSBB.Read(map.Write());Check(back.Parts.Enemies.Count==result.Spawns.Count&&back.Parts.Enemies.All(a=>back.Models.Enemies.Any(m=>m.Name==a.ModelName)),"Enemy MSB references failed serialization");
            total+=result.Spawns.Count;
        }
        foreach(var (p,hash) in originals)Check(SHA256.HashData(File.ReadAllBytes(p)).SequenceEqual(hash),"Source character asset was modified");
        Console.WriteLine($"PASS: {total} ordinary hollow placements, source masks/shared textures, independent native rigs, composed opaque parameters, MSB references, PS4 tiling and {originals.Count} unchanged source assets");
    }
}
