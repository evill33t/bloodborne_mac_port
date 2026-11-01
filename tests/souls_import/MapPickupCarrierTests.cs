// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

// Full pickup composition, including the otherwise independently-correct
// imported corpse map pieces. A static-mesh-only fixture missed this overlay.
static class MapPickupCarrierTests {
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Check(bool b,string why){if(!b)throw new Exception("Pickup carrier: "+why);}
    static string Piece(MSBB.Part.MapPiece p)=>$"{p.Name}|{p.ModelName}|{p.Position}|{p.Rotation}|{p.Scale}";
    public static void Run(string sourceRoot,string nativeRoot,string definitions,string modRoot,string output) {
        var hashes=new Dictionary<string,string>();var staged=new Dictionary<string,byte[]>();
        byte[] Read(string path){var bytes=File.ReadAllBytes(path);hashes.TryAdd(path,Convert.ToHexStringLower(SHA256.HashData(bytes)));return bytes;}
        byte[]? Source(string p)=>File.Exists(Path.Combine(sourceRoot,p))?Read(Path.Combine(sourceRoot,p)):null;
        byte[]? Native(string p)=>staged.GetValueOrDefault(p)??(File.Exists(Path.Combine(modRoot,"dvdroot_ps4",p))?Read(Path.Combine(modRoot,"dvdroot_ps4",p)):File.Exists(Path.Combine(nativeRoot,p))?Read(Path.Combine(nativeRoot,p)):null);
        const string param="param/gameparam/gameparam.parambnd.dcx",owned="obj/o699990.objbnd.dcx";
        var dream=MSBB.Read(Read(Path.Combine(nativeRoot,"map/MapStudio/m21_00_00_00.msb.dcx")));
        var donorTreasure=dream.Events.Treasures.First(t=>!t.InChest&&t.ItemLot1>=0);
        var donorBundle=BND4.Read(Read(Path.Combine(nativeRoot,"obj/o000611.objbnd.dcx")));
        var donor=FLVER2.Read(donorBundle.Files.Single(f=>Leaf(f.Name)=="o000611.flver").Bytes);
        Check(donor.Meshes.Count==2&&donor.Meshes.Sum(m=>m.Vertices.Count)==3691,"fixture does not contain the real native corpse");
        var coverage=new List<object>();int total=0,corpseCarriers=0,staticOverlaps=0,dummyCarriers=0;
        foreach(string reportPath in new[]{"conversion.json","maps/m10_01_00_00/conversion.json"}) {
            using var json=JsonDocument.Parse(Read(Path.Combine(modRoot,reportPath)));var r=json.RootElement;
            string sourceMap=r.GetProperty("map").GetString()!,targetMap=r.GetProperty("target").GetString()!;
            var source=MSB1.Read(Source("map/MapStudio/"+sourceMap+".msb")!);
            var old=MSBB.Read(Native("map/MapStudio/"+targetMap+".msb.dcx")!);
            var oldPickups=r.GetProperty("items").GetProperty("Pickups").EnumerateArray().ToArray();
            var names=oldPickups.Select(p=>p.GetProperty("TargetPart").GetString()!).ToHashSet();
            var offset=old.Parts.Objects.Single(p=>p.Name==oldPickups[0].GetProperty("TargetPart").GetString()).Position-
                source.Parts.GetEntries().Single(p=>p.Name==oldPickups[0].GetProperty("SourcePart").GetString()).Position;
            var originals=old.Parts.MapPieces.Select(Piece).ToArray();
            old.Parts.Objects.RemoveAll(p=>names.Contains(p.Name));old.Events.Treasures.RemoveAll(e=>names.Contains(e.TreasurePartName));
            old.Models.Objects.RemoveAll(m=>m.Name==MapItems.PickupModel);var slot=old.Parts.Collisions.First();
            if(total==0){var before=old.Write();bool rejected=false;try{MapItems.Bake(Source,p=>p==owned?donorBundle.Write(donorBundle.Compression):Native(p),source,old,sourceMap,definitions,nativeRoot,offset,[new(source.Parts.Collisions.First().Name,[],[])],[slot],[],Native(param)!);}catch(InvalidDataException){rejected=true;}Check(rejected&&before.SequenceEqual(old.Write()),"occupied carrier namespace mutated the scene or was accepted");}
            var result=MapItems.Bake(Source,Native,source,old,sourceMap,definitions,nativeRoot,offset,
                [new(source.Parts.Collisions.First().Name,[],[])],[slot],[],Native(param)!);
            if(staged.TryGetValue(owned,out var prior))Check(prior.SequenceEqual(result.Files[owned]),"connected maps generated different owned carriers");
            Check(result.Pickups.Count==oldPickups.Length,"actual source treasure population changed");
            Check(result.PickupCarrier is {DonorModel:"o000611",TargetModel:"o699990",RemovedMeshes:2,RemovedVertices:3691,AttachmentDummy:90},"owned carrier provenance missing");
            Check(result.Files.ContainsKey(owned)&&!result.Files.ContainsKey("obj/o000611.objbnd.dcx"),"native body asset was overwritten");
            var bundle=BND4.Read(result.Files[owned]);Check(bundle.Files.Count==2&&bundle.Files.All(f=>Leaf(f.Name) is "o699990.flver" or "o699990.tpf"),"owned closure contains physics or stale filenames");
            Check(bundle.Files.Single(f=>Leaf(f.Name)=="o699990.tpf").Bytes.Span.SequenceEqual(donorBundle.Files.Single(f=>Leaf(f.Name)=="o000611.tpf").Bytes.Span),"native TPF changed");
            var blank=FLVER2.Read(bundle.Files.Single(f=>Leaf(f.Name)=="o699990.flver").Bytes);Check(blank.Meshes.Count==0,"wrong native body can still render");
            // Restore only mesh topology: every other serialized FLVER field,
            // including dummy90 frame, nodes and materials, must match donor.
            blank.Meshes.AddRange(donor.Meshes);Check(blank.Write().SequenceEqual(donor.Write()),"attachment/header changed in addition to render suppression");
            Check(originals.SequenceEqual(old.Parts.MapPieces.Select(Piece)),"source corpse/static placements changed");
            var round=MSBB.Read(old.Write());var rows=ImportParam.Read(BND4.Read(result.GameParamBytes).Files.Single(f=>Leaf(f.Name)=="ObjectParam.param").Bytes,"OBJECT_PARAM_ST",32,1);
            Check(rows.Rows.All(p=>p.ID is not(611 or 699990)),"default object behavior was replaced by invented parameters");
            foreach(var pickup in result.Pickups) {
                var from=source.Parts.GetEntries().Single(p=>p.Name==pickup.SourcePart);var actor=round.Parts.Objects.Single(p=>p.Name==pickup.TargetPart);
                var evt=round.Events.Treasures.Single(e=>e.TreasurePartName==actor.Name);
                Check(actor.ModelName==MapItems.PickupModel&&actor.Scale==Vector3.One&&Vector3.Distance(actor.Position,from.Position+offset)<.0001f&&actor.Rotation==from.Rotation,"owned pickup transform changed");
                Check(!evt.InChest&&!evt.StartDisabled&&new[]{evt.ItemLot1,evt.ItemLot2,evt.ItemLot3}.Where(i=>i>=0).SequenceEqual(pickup.Lots),"treasure engine contract changed");
                foreach(var property in typeof(MSBB.Event.Treasure).GetProperties().Where(p=>p.CanRead&&!new[]{"Name","EventID","EntityID","TreasurePartName","ItemLot1","ItemLot2","ItemLot3"}.Contains(p.Name)))Check(Equals(property.GetValue(evt),property.GetValue(donorTreasure)),"native treasure flag/action changed: "+property.Name);
                if(from.ModelName is "o0500" or "o0502" or "o0504")corpseCarriers++;
                if(from is MSB1.Part.DummyObject)dummyCarriers++;
                if(round.Parts.MapPieces.Any(p=>Vector3.Distance(p.Position,actor.Position)<.0001f))staticOverlaps++;
            }
            foreach(var pair in result.Files)staged[pair.Key]=pair.Value;
            total+=result.Pickups.Count;coverage.Add(new{sourceMap,pickups=result.Pickups.Count,result.PickupCarrier});
        }
        Check(total==32&&corpseCarriers==31&&staticOverlaps==29&&dummyCarriers==2,"real composed overlay coverage changed");
        foreach(var(path,hash) in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==hash,"source/native/export asset modified");
        Directory.CreateDirectory(output);File.WriteAllText(Path.Combine(output,"pickup-carrier.json"),JsonSerializer.Serialize(new{status="PASS",total,corpseCarriers,staticOverlaps,dummyCarriers,coverage,originalAssetsPreserved=hashes.Count,limit="Native glow/pickup/save interaction after visual suppression requires gameplay validation."},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS: {total} full-composition pickups/{corpseCarriers} corpse carriers use an owned meshless object, native dummy/treasures and all source pieces retained; {hashes.Count} original assets unchanged");
    }
}
