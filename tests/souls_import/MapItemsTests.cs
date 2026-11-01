// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class MapItemsTests {
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Check(bool ok,string reason){if(!ok)throw new Exception(reason);}
    static void Reject(Action action,string reason){try{action();}catch(InvalidDataException){return;}throw new Exception(reason);}
    static byte[] Data(PARAM.Row row)=>ImportParam.Payload(row);
    static int Int(byte[] bytes,int at)=>BitConverter.ToInt32(bytes,at);
    static PARAM Table(BND4 bank,string name,string type,int size,short version)=>ImportParam.Read(bank.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes,type,size,version);
    static int Field(PARAM.Row row,string name)=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName==name).Value);
    public static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],definitions=args[2],modRoot=args[3],output=args[4];
        var hashes=new Dictionary<string,string>(StringComparer.Ordinal);var staged=new Dictionary<string,byte[]>(StringComparer.Ordinal);
        byte[] Read(string path){var bytes=File.ReadAllBytes(path);hashes.TryAdd(path,Convert.ToHexStringLower(SHA256.HashData(bytes)));return bytes;}
        byte[]? Source(string path)=>File.Exists(Path.Combine(sourceRoot,path))?Read(Path.Combine(sourceRoot,path)):null;
        byte[]? Native(string path)=>staged.GetValueOrDefault(path)??
            (File.Exists(Path.Combine(modRoot,"dvdroot_ps4",path))?Read(Path.Combine(modRoot,"dvdroot_ps4",path)):
            File.Exists(Path.Combine(nativeRoot,path))?Read(Path.Combine(nativeRoot,path)):null);
        const string paramPath="param/gameparam/gameparam.parambnd.dcx";
        byte[] baseline=Native(paramPath)??throw new Exception("Actual connected export parameters missing");var baselineHash=SHA256.HashData(baseline);
        var reports=new List<(string Map,List<MapEnemySpawns.Spawn> Actors)>();
        foreach(string path in new[]{"conversion.json","maps/m10_01_00_00/conversion.json"}) {
            using var json=JsonDocument.Parse(Read(Path.Combine(modRoot,path)));var root=json.RootElement;string map=root.GetProperty("map").GetString()!;
            var target=MSBB.Read(Native("map/MapStudio/"+root.GetProperty("target").GetString()+".msb.dcx")!);
            var actors=new List<MapEnemySpawns.Spawn>();
            foreach(var p in root.GetProperty("enemySpawns").GetProperty("Spawns").EnumerateArray()) {
                int N(string key)=>p.GetProperty(key).GetInt32();string S(string key)=>p.GetProperty(key).GetString()!;
                var actor=new MapEnemySpawns.Spawn(S("SourceName"),S("SourceModel"),N("SourceNPCParamID"),N("SourceThinkParamID"),N("SourceEntityID"),S("TargetModel"),N("EntityID"),N("NPCParamID"),N("ThinkParamID"),Vector3.Zero,p.GetProperty("DisplayMasks").EnumerateArray().Select(v=>v.GetInt32()).ToArray(),1,1,[],p.GetProperty("HasSourceTorch").GetBoolean(),N("SourceInitAnimID"));
                var placed=target.Parts.Enemies.Single(a=>a.EntityID==actor.EntityID);
                Check(placed.ModelName==actor.TargetModel&&placed.NPCParamID==actor.NPCParamID,"Final native MSB differs from its imported enemy identity");actors.Add(actor);
            }
            reports.Add((map,actors));
        }
        var all=reports.SelectMany(p=>p.Actors).ToArray();Check(all.Length==113&&all.Select(p=>p.EntityID).Distinct().Count()==113,"Actual connected export enemy population changed");
        Check(all.Count(p=>p.SourceModel is "c2900" or "c2910")==13&&all.Count(p=>p.SourceModel is "c2250" or "c2240" or "c5350")==4&&all.Count(p=>p.SourceModel=="c3340")==2&&all.Count(p=>p.SourceModel is "c3460" or "c2300")==2,"Actual special/boss/dog coverage changed");
        var original=BND4.Read(baseline);var originalNpcs=Table(original,"NpcParam","NPC_PARAM_ST",388,5);
        foreach(var actor in all)Check(Enumerable.Range(0,6).Count(i=>Int(Data(originalNpcs.Rows.Single(r=>r.ID==actor.NPCParamID)),44+i*4)==MapItems.VialLot)==1,"Later final-export composition lost the shared vial lot: "+actor.SourceName);
        var donorMap=MSBB.Read(Read(Path.Combine(nativeRoot,"map/MapStudio/m24_01_00_01.msb.dcx")));var slot=donorMap.Parts.Collisions.First();
        MapItems.Result Bake(string map,IReadOnlyList<MapEnemySpawns.Spawn> actors,byte[] input) {
            var source=MSB1.Read(Source("map/MapStudio/"+map+".msb")!);
            // Only parameter composition is under test here. Pickup placement
            // has the complete installed-map fixture; omit its repeated work.
            source.Events.Treasures.Clear();var destination=new MSBB();destination.Parts.Collisions.Add((MSBB.Part.Collision)slot.DeepCopy());
            destination.Models.Collisions.Add((MSBB.Model.Collision)donorMap.Models.Collisions.Single(m=>m.Name==slot.ModelName).DeepCopy());
            return MapItems.Bake(Source,Native,source,destination,map,definitions,nativeRoot,Vector3.Zero,[new(source.Parts.Collisions.First().Name,[],[])],[slot],actors,input);
        }
        byte[] composed=baseline;var passes=new List<object>();
        for(int pass=0;pass<2;pass++)foreach(var (map,actors) in reports) {
            var result=Bake(map,actors,composed);Check(result.VialDropLot==89000000,"Shared imported-enemy vial identity shifted");composed=result.GameParamBytes;
            foreach(var pair in result.Files)staged[pair.Key]=pair.Value;
            passes.Add(new{pass,map,actors=actors.Count,parameters=Convert.ToHexStringLower(SHA256.HashData(composed))});
            if(pass==0&&map==reports[^1].Map)staged["fixture/first-pass"]=composed;
        }
        Check(composed.SequenceEqual(staged["fixture/first-pass"]),"Connected item recomposition is not byte-idempotent");
        Check(SHA256.HashData(baseline).SequenceEqual(baselineHash),"Item composition mutated supplied parameter bytes");
        var game=BND4.Read(composed);var npcs=Table(game,"NpcParam","NPC_PARAM_ST",388,5);var lots=Table(game,"ItemLotParam","ITEMLOT_PARAM_ST",148,2);
        var vial=lots.Rows.Single(r=>r.ID==MapItems.VialLot);var payload=Data(vial);
        Check(vial.Name=="Imported enemy Blood Vial"&&payload.SequenceEqual(MapItems.LotBytes([(1000,1,100),(0,0,900)])),"Vial lot differs from its fixed ten-percent repeatable definition");
        var typedLots=PARAM.Read(game.Files.Single(f=>Leaf(f.Name)=="ItemLotParam.param").Bytes);
        typedLots.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs/ItemLotParam.xml"))));
        var typed=typedLots.Rows.Single(r=>r.ID==MapItems.VialLot);
        Check(Field(typed,"lotItemId01")==1000&&Field(typed,"lotItemCategory01")==4&&Field(typed,"lotItemNum01")==1&&Field(typed,"lotItemBasePoint01")==100&&
            Field(typed,"lotItemId02")==0&&Field(typed,"lotItemCategory02")==-1&&Field(typed,"lotItemNum02")==0&&Field(typed,"lotItemBasePoint02")==900&&
            Field(typed,"getItemFlagId")==-1&&Field(typed,"cumulateNumFlagId")==-1&&Field(typed,"cumulateNumMax")==0,"Installed native ItemLot fields do not express one-vial100:none900");
        for(int i=1;i<=8;i++) {
            string suffix=i.ToString("D2");Check(Field(typed,"enableLuck"+suffix)==0&&Field(typed,"cumulateReset"+suffix)==0&&Field(typed,"cumulateLotPoint"+suffix)==0&&Field(typed,"getItemFlagId"+suffix)==0,"Discovery/cumulative/collection behavior changes the fixed vial chance");
            if(i>2)Check(Field(typed,"lotItemBasePoint"+suffix)==0,"An undeclared vial outcome has nonzero weight");
        }
        var owned=all.Select(p=>p.NPCParamID).ToHashSet();var coverage=new List<object>();
        foreach(var actor in all) {
            var row=npcs.Rows.Single(r=>r.ID==actor.NPCParamID);var data=Data(row);var refs=Enumerable.Range(0,6).Select(i=>Int(data,44+i*4)).ToArray();
            Check(refs.Count(id=>id==MapItems.VialLot)==1&&Int(data,68)==-1,"An imported enemy lacks exactly one independent repeatable vial roll");
            coverage.Add(new{actor.SourceName,actor.SourceModel,actor.EntityID,actor.NPCParamID,lots=refs});
        }
        foreach(var (table,type,size,version) in new[]{("NpcParam","NPC_PARAM_ST",388,(short)5),("ItemLotParam","ITEMLOT_PARAM_ST",148,(short)2),("EquipParamGoods","EQUIP_PARAM_GOODS_ST",108,(short)1)}) {
            var before=Table(original,table,type,size,version);var after=Table(game,table,type,size,version);
            Check(before.Rows.Count==after.Rows.Count,"Unexpected new rows in an already-complete actual export");
            for(int i=0;i<before.Rows.Count;i++) {
                var a=before.Rows[i];var b=after.Rows[i];Check(a.ID==b.ID,"Native/owned row order changed");
                if(table=="ItemLotParam"&&a.ID==MapItems.VialLot)continue;
                Check(a.Name==b.Name,"Original null/row name changed");var x=Data(a);var y=Data(b);
                if(table=="NpcParam"&&owned.Contains(a.ID)) {for(int at=0;at<x.Length;at++)if(at is <44 or >=72)Check(x[at]==y[at],"Owned enemy changed a non-loot byte");}
                else Check(x.SequenceEqual(y),"Native/friendly/quest/equipment row changed");
            }
        }
        Check(new[]{890000,890100,890200}.All(id=>npcs.Rows.Any(r=>r.ID==id)&&!owned.Contains(id)),"Friendly NPC exclusion fixture is incomplete");
        foreach(var f in original.Files.Where(f=>Leaf(f.Name) is not("NpcParam.param" or "ItemLotParam.param" or "EquipParamGoods.param")))
            Check(game.Files.Single(a=>a.ID==f.ID&&a.Name==f.Name).Bytes.Span.SequenceEqual(f.Bytes.Span),"Unrelated composed parameter file changed");
        byte[] Alter(Action<PARAM> action) {var bank=BND4.Read(composed);var rows=Table(bank,"ItemLotParam","ITEMLOT_PARAM_ST",148,2);action(rows);bank.Files.Single(f=>Leaf(f.Name)=="ItemLotParam.param").Bytes=ImportParam.Write(rows);return bank.Write(DCX.Type.DCX_DFLT_10000_44_9);}
        var first=reports[0];var legacy=Alter(p=>{var r=p.Rows.Single(r=>r.ID==MapItems.VialLot);r.Name="Imported ordinary hollow Blood Vial";r.Cells.Single().Value=MapItems.LotBytes([(1000,1,300),(0,0,700)]);});
        Check(Data(Table(BND4.Read(Bake(first.Map,first.Actors,legacy).GameParamBytes),"ItemLotParam","ITEMLOT_PARAM_ST",148,2).Rows.Single(r=>r.ID==MapItems.VialLot)).SequenceEqual(payload),"Exact legacy owned vial row did not migrate");
        var absent=Alter(p=>p.Rows.RemoveAll(r=>r.ID==MapItems.VialLot));
        Check(Data(Table(BND4.Read(Bake(first.Map,first.Actors,absent).GameParamBytes),"ItemLotParam","ITEMLOT_PARAM_ST",148,2).Rows.Single(r=>r.ID==MapItems.VialLot)).SequenceEqual(payload),"Fresh shared vial row was not created");
        Reject(()=>Bake(first.Map,first.Actors,Alter(p=>p.Rows.Single(r=>r.ID==MapItems.VialLot).Name="Unrelated custom lot")),"An unrelated reserved row was overwritten");
        Reject(()=>Bake(first.Map,first.Actors,Alter(p=>p.Rows.Single(r=>r.ID==MapItems.VialLot).Cells.Single().Value=MapItems.LotBytes([(1000,1,999),(0,0,1)]))),"An altered owned-name payload was silently overwritten");
        Reject(()=>Bake(first.Map,[first.Actors[0] with{TargetModel="c8900",NPCParamID=890000}],composed),"Friendly NPC loot could be overwritten");
        foreach(var (file,hash) in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(file)))==hash,"Original source/native/export asset changed: "+file);
        MapPickupCarrierTests.Run(sourceRoot,nativeRoot,definitions,modRoot,output);
        Directory.CreateDirectory(output);File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{status="PASS",actors=all.Length,npcRows=owned.Count,weights=new[]{100,900},count=1,luck=false,cumulative=false,repeatable=true,passes,coverage,originalHashes=hashes,nativePickup="Not exercised; deterministic schema and complete actual-export coverage only"},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS: all113 actual-export enemies/{owned.Count} NPC rows, one-vial10%:none90%, no luck/cumulation, friendly/native/source loot preserved, connected byte-idempotence, legacy migration/conflict guards; {hashes.Count} original assets unchanged");
    }
}
