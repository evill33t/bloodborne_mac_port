// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class MapItems {
    internal record Lot(int SourceID,int TargetID,int SourceCollectionFlag,int CollectionFlag,IReadOnlyList<MapItemPolicy.Conversion> Goods,int IgnoredEquipment);
    internal record Pickup(string SourceName,string SourcePart,string TargetPart,int[] Lots,Vector3 Position);
    internal record Omitted(string SourceName,string Reason);
    internal record Carrier(string DonorModel,string TargetModel,int RemovedMeshes,int RemovedVertices,int AttachmentDummy);
    internal record Result(byte[] GameParamBytes,IReadOnlyDictionary<string,byte[]> Files,IReadOnlyList<Lot> Lots,
        IReadOnlyList<Pickup> Pickups,IReadOnlyList<Omitted> Skipped,IReadOnlyDictionary<int,int> QuestItems,int VialDropLot) {
        public Carrier? PickupCarrier {get;init;}
        public IReadOnlyList<string> Limits {get;init;}=[];
    }
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static int Number(PARAM.Row row,string field)=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName==field).Value);
    static void Int(byte[] data,int at,int value)=>BitConverter.GetBytes(value).CopyTo(data,at);
    internal const int VialLot=89000000;
    internal const string PickupModel="o699990";
    const string VialOwner="Imported enemy Blood Vial",LegacyVialOwner="Imported ordinary hollow Blood Vial";
    static void NativeLootSchema(string definitions) {
        string root=Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs"));
        var lot=PARAMDEF.XmlDeserialize(Path.Combine(root,"ItemLotParam.xml"));
        var npc=PARAMDEF.XmlDeserialize(Path.Combine(root,"NpcParam.xml"));
        void Field(PARAMDEF def,string name,int offset,PARAMDEF.DefType type) {
            int index=def.Fields.FindIndex(f=>f.InternalName==name);if(index<0)throw new InvalidDataException("Native loot field is missing: "+name);
            var prefix=new PARAMDEF();prefix.Fields.AddRange(def.Fields.Take(index));var field=def.Fields[index];
            if(prefix.GetRowSize()!=offset||field.DisplayType!=type||field.ArrayLength!=1||field.BitSize!=-1)
                throw new InvalidDataException("Native loot field layout changed: "+name);
        }
        if(lot.ParamType!="ITEMLOT_PARAM_ST"||lot.DataVersion!=2||lot.GetRowSize()!=148||npc.ParamType!="NPC_PARAM_ST"||npc.DataVersion!=5||npc.GetRowSize()!=388)
            throw new InvalidDataException("Native loot parameter schema changed");
        for(int i=0;i<8;i++) {
            string suffix=(i+1).ToString("D2");Field(lot,"lotItemId"+suffix,i*4,PARAMDEF.DefType.s32);
            Field(lot,"lotItemCategory"+suffix,32+i*4,PARAMDEF.DefType.s32);Field(lot,"lotItemBasePoint"+suffix,64+i*2,PARAMDEF.DefType.u16);
            Field(lot,"cumulateLotPoint"+suffix,80+i*2,PARAMDEF.DefType.u16);Field(lot,"getItemFlagId"+suffix,96+i*4,PARAMDEF.DefType.s32);
            Field(lot,"lotItemNum"+suffix,138+i,PARAMDEF.DefType.u8);
        }
        Field(lot,"getItemFlagId",128,PARAMDEF.DefType.s32);Field(lot,"cumulateNumFlagId",132,PARAMDEF.DefType.s32);
        Field(lot,"cumulateNumMax",136,PARAMDEF.DefType.u8);Field(lot,"lotItem_Rarity",137,PARAMDEF.DefType.u8);
        var flags=lot.Fields.TakeLast(16).ToArray();
        var names=Enumerable.Range(1,8).Select(i=>"enableLuck"+i.ToString("D2")).Concat(Enumerable.Range(1,8).Select(i=>"cumulateReset"+i.ToString("D2")));
        if(!flags.Select(f=>f.InternalName).SequenceEqual(names)||flags.Any(f=>f.DisplayType!=PARAMDEF.DefType.u16||f.BitSize!=1||f.ArrayLength!=1))
            throw new InvalidDataException("Native loot luck/cumulation bit layout changed");
        for(int i=0;i<6;i++)Field(npc,"itemLotId_"+(i+1),44+i*4,PARAMDEF.DefType.s32);
        Field(npc,"humanityLotId",68,PARAMDEF.DefType.s32);
    }
    internal static byte[] LotBytes(IReadOnlyList<(int Item,int Count,int Weight)> entries,int flag=-1) {
        if(entries.Count is <1 or >8||entries.Any(e=>e.Item<0||e.Count<0||e.Count>99||e.Weight<0||e.Weight>65535)||entries.Sum(e=>e.Weight)<=0)
            throw new InvalidDataException("Invalid native imported item lot");
        var bytes=new byte[148];Int(bytes,128,flag);Int(bytes,132,-1);
        for(int i=0;i<entries.Count;i++) {
            var e=entries[i];Int(bytes,i*4,e.Item);Int(bytes,32+i*4,e.Item==0?-1:MapItemPolicy.NativeGoodsCategory);
            BitConverter.GetBytes(checked((ushort)e.Weight)).CopyTo(bytes,64+i*2);bytes[138+i]=checked((byte)e.Count);
        }
        return bytes;
    }
    const int FirstFlag=52100600,LastFlag=52100899;
    static HashSet<int> ReservedFlags(BND4 game,string root) {
        var used=new HashSet<int>();
        void Scan(ReadOnlySpan<byte> bytes) {
            for(int at=0;at+4<=bytes.Length;at++) {
                int value=System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(bytes[at..]);
                if(value>=FirstFlag&&value<=LastFlag)used.Add(value);
            }
        }
        foreach(var file in game.Files)Scan(file.Bytes.Span);
        foreach(var folder in new[]{"event","script/talk"})foreach(var file in Directory.EnumerateFiles(Path.Combine(root,folder),"*.dcx")) {
            var bytes=File.ReadAllBytes(file);ReadOnlyMemory<byte> unpacked=DCX.Is(bytes)?DCX.Decompress(bytes):bytes;Scan(unpacked.Span);
        }
        return used;
    }
    internal static Result Bake(Func<string,byte[]?> sourceRead,Func<string,byte[]?> nativeRead,MSB1 source,MSBB destination,
        string sourceMap,string definitions,string nativeRoot,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        IReadOnlyList<MapEnemySpawns.Spawn> enemies,byte[] gameParamBytes,IReadOnlyList<int>? rewardLots=null) {
        if(chunks.Count!=slots.Count||slots.Count==0)throw new InvalidDataException("Item collision bindings differ");
        NativeLootSchema(definitions);
        var sourceFiles=BND3.Read(sourceRead("param/GameParam/GameParam.parambnd.dcx")??throw new FileNotFoundException("Source item parameters unavailable")).Files;
        PARAM Source(string name){var p=PARAM.Read(sourceFiles.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,name+".xml")));return p;}
        var sourceGoods=Source("EquipParamGoods");var sourceLots=Source("ItemLotParam");var sourceNpcs=Source("NpcParam");
        var game=BND4.Read(gameParamBytes);BinderFile Entry(string name)=>game.Files.Single(f=>Leaf(f.Name)==name+".param");
        var lots=ImportParam.Read(Entry("ItemLotParam").Bytes,"ITEMLOT_PARAM_ST",148,2);
        var goods=ImportParam.Read(Entry("EquipParamGoods").Bytes,"EQUIP_PARAM_GOODS_ST",108,1);
        var npcs=ImportParam.Read(Entry("NpcParam").Bytes,"NPC_PARAM_ST",388,5);
        var goodsTyped=PARAM.Read(Entry("EquipParamGoods").Bytes);
        goodsTyped.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs/EquipParamGoods.xml"))));
        var files=new Dictionary<string,byte[]>();var reports=new List<Lot>();var pickups=new List<Pickup>();var skipped=new List<Omitted>();var questItems=new Dictionary<int,int>();
        var sourceRows=sourceLots.Rows.ToDictionary(r=>r.ID);var converted=new Dictionary<int,Lot?>();
        var flags=new Dictionary<int,int>();var reserved=ReservedFlags(game,nativeRoot);
        // Reuse ownership from an earlier composed map, including a shared source flag.
        foreach(var old in lots.Rows.Where(r=>r.Name?.StartsWith("Imported DS1R lot ",StringComparison.Ordinal)==true)) {
            if(!sourceRows.TryGetValue(old.ID-90000000,out var original))throw new InvalidDataException("Composed imported item lot has no source row");
            int sourceFlag=Number(original,"getItemFlagId"),nativeFlag=BitConverter.ToInt32(ImportParam.Payload(old),128);
            if(sourceFlag<=0)continue;
            if(nativeFlag<FirstFlag||nativeFlag>LastFlag||flags.TryGetValue(sourceFlag,out int previousFlag)&&previousFlag!=nativeFlag)
                throw new InvalidDataException("Composed imported item flag ownership conflicts");
            flags[sourceFlag]=nativeFlag;
        }
        int CollectionFlag(int sourceFlag) {
            if(sourceFlag<=0)return sourceFlag;
            if(sourceFlag is <50000000 or >=60000000)throw new NotSupportedException("Source collection flag requires a quest adapter: "+sourceFlag);
            if(flags.TryGetValue(sourceFlag,out int existingFlag))return existingFlag;
            int fresh=Enumerable.Range(FirstFlag,LastFlag-FirstFlag+1).FirstOrDefault(f=>!reserved.Contains(f),-1);
            if(fresh<0)throw new InvalidDataException("Imported pickup retail flag reservation is exhausted");
            reserved.Add(fresh);flags.Add(sourceFlag,fresh);return fresh;
        }
        MapItemPolicy.Conversion? ConvertGood(int category,int id,int count) {
            if(category!=MapItemPolicy.SourceGoodsCategory)return null;
            var sourceRow=sourceGoods.Rows.SingleOrDefault(r=>r.ID==id);if(sourceRow==null)return null;
            var result=MapItemPolicy.Map(category,id,count,MapItemPolicy.IsQuest(sourceRow));if(result==null)return null;
            if(result.Kind=="quest-item") {
                string owner="Imported DS1R good "+id;var existing=goods.Rows.SingleOrDefault(r=>r.ID==result.TargetID);
                if(existing!=null&&existing.Name!=owner)throw new InvalidDataException("Imported quest item namespace is occupied: "+result.TargetID);
                if(existing==null) {
                    var donor=goods.Rows.Single(r=>r.ID==4000);
                    var key=new PARAM.Row(goodsTyped.Rows.Single(r=>r.ID==4000)){ID=result.TargetID,Name=owner};
                    key.Cells.Single(c=>c.Def.InternalName=="maxNum").Value=checked((short)Math.Clamp(Number(sourceRow,"maxNum"),1,99));
                    // Inventory identity only: quest adapters decide when to remove a key.
                    key.Cells.Single(c=>c.Def.InternalName=="isConsume").Value=(byte)0;
                    goodsTyped.Rows.Add(key);
                    var encoded=ImportParam.Read(goodsTyped.Write(),"EQUIP_PARAM_GOODS_ST",108,1);
                    ImportParam.Append(goods,donor,result.TargetID,owner,ImportParam.Payload(encoded.Rows.Single(r=>r.ID==result.TargetID)));
                }
                questItems.TryAdd(id,result.TargetID);
            } else if(!goods.Rows.Any(r=>r.ID==result.TargetID))throw new InvalidDataException("Native consumable equivalent is unavailable: "+result.TargetID);
            return result;
        }
        Lot? ConvertLot(int sourceID) {
            if(converted.TryGetValue(sourceID,out var previous))return previous;
            if(!sourceRows.TryGetValue(sourceID,out var row)){converted.Add(sourceID,null);return null;}
            var entries=new List<(int Item,int Count,int Weight)>();var mapped=new List<MapItemPolicy.Conversion>();int equipment=0;
            for(int i=1;i<=8;i++) {
                string suffix=i.ToString("D2");int weight=Number(row,"lotItemBasePoint"+suffix),id=Number(row,"lotItemId"+suffix),category=Number(row,"lotItemCategory"+suffix);
                if(weight==0)continue;
                if(Number(row,"getItemFlagId"+suffix)>0)throw new NotSupportedException("Per-outcome collection conditions require a quest adapter: "+sourceID);
                var good=ConvertGood(category,id,Number(row,"lotItemNum"+suffix));
                if(good!=null)mapped.Add(good);else if(id>0&&category!=MapItemPolicy.SourceGoodsCategory)equipment++;
                entries.Add((good?.TargetID??0,good?.Count??0,weight));
            }
            if(entries.Count==0)entries.Add((0,0,1000));
            int idTarget=checked(90000000+sourceID);if(sourceID<0||sourceID>99999999)throw new InvalidDataException("Source item lot exceeds its namespace");
            int sourceFlag=Number(row,"getItemFlagId"),flag=CollectionFlag(sourceFlag);var bytes=LotBytes(entries,flag);string name="Imported DS1R lot "+sourceID;
            var existing=lots.Rows.SingleOrDefault(r=>r.ID==idTarget);
            if(existing==null)ImportParam.Append(lots,lots.Rows.First(),idTarget,name,bytes);
            else if(existing.Name!=name||!ImportParam.Payload(existing).SequenceEqual(bytes))throw new InvalidDataException("Imported item lot namespace is occupied: "+idTarget);
            var report=new Lot(sourceID,idTarget,sourceFlag,flag,mapped,equipment);reports.Add(report);converted.Add(sourceID,report);return report;
        }
        Lot? ConvertGroup(int sourceID) {
            var group=new List<Lot>();
            for(int id=sourceID;sourceRows.ContainsKey(id);id++) {
                if(group.Count>=64)throw new InvalidDataException("Source item-lot group exceeds its import budget");
                if(ConvertLot(id) is {} row)group.Add(row);
            }
            return group.Any(r=>r.Goods.Count>0)?group[0]:null;
        }
        // Keep standard consumable support independent of equipment conversion.
        // Explicit 10% one-vial / 90% no-drop roll on every adapted enemy.
        // Source loot rolls stay separate; zero luck/cumulation bits preserve
        // the same repeatable chance rather than scaling with item discovery.
        if(goods.Rows.Count(r=>r.ID==MapItemPolicy.BloodVial)!=1)throw new InvalidDataException("Native Blood Vial goods row is unavailable");
        const int vialLot=VialLot;var vialBytes=LotBytes([(MapItemPolicy.BloodVial,1,100),(0,0,900)]);
        var existingVial=lots.Rows.SingleOrDefault(r=>r.ID==vialLot);
        if(existingVial==null)ImportParam.Append(lots,lots.Rows.First(),vialLot,VialOwner,vialBytes);
        else {
            var previous=ImportParam.Payload(existingVial);
            bool current=(existingVial.Name==VialOwner||existingVial.Name==LegacyVialOwner)&&previous.SequenceEqual(vialBytes);
            bool legacy=existingVial.Name==LegacyVialOwner&&previous.SequenceEqual(LotBytes([(MapItemPolicy.BloodVial,1,300),(0,0,700)]));
            if(!current&&!legacy)throw new InvalidDataException("Blood Vial drop namespace is occupied");
            existingVial.Name=VialOwner;existingVial.Cells.Single().Value=vialBytes;
        }
        foreach(var group in enemies.GroupBy(e=>e.NPCParamID)) {
            var actor=group.First();var row=npcs.Rows.Single(r=>r.ID==actor.NPCParamID);
            if(group.Select(p=>p.SourceNPCParamID).Distinct().Count()!=1||group.Any(p=>p.TargetModel!=actor.TargetModel)||
                !System.Text.RegularExpressions.Regex.IsMatch(actor.TargetModel,@"\Ac\d{4}\z")||actor.NPCParamID/100!=int.Parse(actor.TargetModel[1..])||
                row.Name==null||!new[]{"Imported hollow ","Imported cemetery skeleton ","Imported Parish enemy ","Imported boss encounter actor "}.Any(prefix=>row.Name.StartsWith(prefix,StringComparison.Ordinal)))
                throw new InvalidDataException("Blood Vial drop owner is not an adapted enemy: "+actor.NPCParamID);
            var original=sourceNpcs.Rows.Single(r=>r.ID==actor.SourceNPCParamID);
            var selected=Enumerable.Range(1,6).Select(i=>Number(original,"itemLotId_"+i)).Where(id=>id>0).Select(ConvertGroup).Where(r=>r!=null).Select(r=>r!.TargetID).Distinct().ToList();
            if(selected.Count>=6)throw new InvalidDataException("Source enemy has no slot left for its native Blood Vial roll");selected.Add(vialLot);
            var payload=ImportParam.Payload(row);
            for(int i=0;i<6;i++)Int(payload,44+i*4,i<selected.Count?selected[i]:-1);
            Int(payload,68,-1);row.Cells.Single().Value=payload;
        }
        var dream=MSBB.Read(nativeRead("map/MapStudio/m21_00_00_00.msb.dcx")??throw new FileNotFoundException("Native pickup donor unavailable"));
        var treasureTemplate=dream.Events.Treasures.First(t=>!t.InChest&&t.ItemLot1>=0);
        var donorPart=dream.Parts.GetEntries().Single(p=>p.Name==treasureTemplate.TreasurePartName);var donorCopy=donorPart.DeepCopy();
        var objectTemplate=new MSBB.Part.Object();foreach(var property in typeof(MSBB.Part.ObjectBase).GetProperties().Where(p=>p.CanRead&&p.CanWrite))property.SetValue(objectTemplate,property.GetValue(donorCopy));
        Carrier? carrierReport=null;
        void PrepareCarrier() {
            if(carrierReport!=null)return;
            // The Dream donor is a real Yharnam corpse. Its 3,691 vertices
            // previously overlaid every already-baked DS1 loot body. Retain
            // the pickup actor and dummy90 while giving it no render meshes.
            // No HKX/rigid bodies exist in this donor, so this cannot leave an
            // invisible native corpse collider behind the source geometry.
            const string donorModel="o000611";string path="obj/"+PickupModel+".objbnd.dcx";
            if(objectTemplate.ModelName!=donorModel||donorPart is not MSBB.Part.DummyObject)
                throw new InvalidDataException("Native pickup actor donor changed");
            var objectRows=ImportParam.Read(Entry("ObjectParam").Bytes,"OBJECT_PARAM_ST",32,1);
            if(objectRows.Rows.Any(r=>r.ID is 611 or 699990))throw new InvalidDataException("Pickup default ObjectParam contract or owned namespace changed");
            var bytes=nativeRead("obj/"+donorModel+".objbnd.dcx")??throw new FileNotFoundException("Native pickup visual donor unavailable");var bundle=BND4.Read(bytes);
            if(bundle.Files.Count!=2||!bundle.Files.Select(f=>Leaf(f.Name)).Order(StringComparer.Ordinal).SequenceEqual(new[]{donorModel+".flver",donorModel+".tpf"}))
                throw new InvalidDataException("Native pickup donor has unexpected physics/asset dependencies");
            var visual=bundle.Files.Single(f=>Leaf(f.Name)==donorModel+".flver");var flver=FLVER2.Read(visual.Bytes);
            if(flver.Meshes.Count!=2||flver.Meshes.Sum(m=>m.Vertices.Count)!=3691||flver.Dummies.Count!=1||flver.Dummies[0].ReferenceID!=90)
                throw new InvalidDataException("Native pickup mesh/attachment contract changed");
            carrierReport=new(donorModel,PickupModel,flver.Meshes.Count,flver.Meshes.Sum(m=>m.Vertices.Count),90);flver.Meshes.Clear();visual.Bytes=flver.Write();
            if(FLVER2.Read(visual.Bytes).Meshes.Count!=0)throw new InvalidDataException("Owned pickup still has visible meshes");
            foreach(var file in bundle.Files)file.Name=file.Name.Replace(donorModel,PickupModel,StringComparison.Ordinal);
            var output=bundle.Write(DCX.Type.DCX_DFLT_10000_44_9);var existing=nativeRead(path);
            if(existing!=null&&!existing.SequenceEqual(output))throw new InvalidDataException("Owned invisible pickup model is occupied");
            var model=(MSBB.Model.Object)dream.Models.Objects.Single(m=>m.Name==donorModel).DeepCopy();model.Name=PickupModel;
            model.SibPath=@"N:\SPRJ\data\Model\obj\"+PickupModel+@"\sib\"+PickupModel+".sib";
            if(destination.Models.Objects.Any(m=>m.Name==PickupModel))throw new InvalidDataException("Owned pickup MSB model is occupied");
            files.Add(path,output);destination.Models.Objects.Add(model);objectTemplate.ModelName=PickupModel;
        }
        int firstEntity=sourceMap switch {"m10_02_00_00"=>2418500,"m10_01_00_00"=>2408500,_=>throw new NotSupportedException("No item entity reservation for "+sourceMap)};
        int entity=firstEntity;
        int eventID=destination.Events.GetEntries().Select(e=>e.EventID).DefaultIfEmpty(-1).Max()+1;
        var entities=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Events.GetEntries().Select(e=>e.EntityID)).Concat(destination.Regions.GetEntries().Select(r=>r.EntityID)).ToHashSet();
        var names=destination.Parts.GetEntries().Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        foreach(var treasure in source.Events.Treasures) {
            if(treasure.InChest||treasure.StartDisabled){skipped.Add(new(treasure.Name,"Chest/conditional treasure requires its interaction or quest activation adapter."));continue;}
            var carrier=source.Parts.GetEntries().SingleOrDefault(p=>p.Name==treasure.TreasurePartName);
            if(carrier==null){skipped.Add(new(treasure.Name,"Source pickup carrier is missing."));continue;}
            var selected=treasure.ItemLots.Where(id=>id>=0).Select(ConvertGroup).Where(r=>r!=null).Select(r=>r!.TargetID).ToArray();
            if(selected.Length==0){skipped.Add(new(treasure.Name,"No supported consumable or quest item; equipment is deliberately excluded."));continue;}
            if(selected.Length>3)throw new InvalidDataException("Source treasure exceeds the native item-lot slot count");
            PrepareCarrier();
            while(entities.Contains(entity))entity++;
            if(entity>=firstEntity+100)throw new InvalidDataException("Imported pickup entity reservation is exhausted");
            var obj=(MSBB.Part.Object)objectTemplate.DeepCopy();obj.InstanceID=9000+pickups.Count;obj.Name=obj.ModelName+"_"+obj.InstanceID.ToString("D4");obj.EntityID=entity++;entities.Add(obj.EntityID);obj.Position=carrier.Position+offset;obj.Rotation=carrier.Rotation;obj.Scale=Vector3.One;
            if(!names.Add(obj.Name))throw new InvalidDataException("Imported pickup name is occupied: "+obj.Name);
            var sourceCollision=(carrier as MSB1.Part.ObjectBase)?.CollisionName;
            int collision=Enumerable.Range(0,chunks.Count).FirstOrDefault(i=>chunks[i].SourceName==sourceCollision,0);
            obj.CollisionName=slots[collision].Name;
            Array.Clear(obj.DrawGroups);Array.Copy(carrier.DrawGroups,obj.DrawGroups,carrier.DrawGroups.Length);
            Array.Clear(obj.DispGroups);Array.Copy(carrier.DispGroups,obj.DispGroups,carrier.DispGroups.Length);
            Array.Clear(obj.BackreadGroups);Array.Copy(carrier.DrawGroups,obj.BackreadGroups,carrier.DrawGroups.Length);destination.Parts.Objects.Add(obj);
            var evt=(MSBB.Event.Treasure)treasureTemplate.DeepCopy();evt.Name="souls_treasure_"+pickups.Count.ToString("D4");evt.EventID=eventID++;evt.EntityID=-1;evt.TreasurePartName=obj.Name;evt.StartDisabled=false;
            evt.ItemLot1=selected[0];evt.ItemLot2=selected.Length>1?selected[1]:-1;evt.ItemLot3=selected.Length>2?selected[2]:-1;destination.Events.Treasures.Add(evt);
            pickups.Add(new(treasure.Name,carrier.Name,obj.Name,selected,obj.Position));
        }
        foreach(int reward in rewardLots??[])if(ConvertGroup(reward)==null)throw new NotSupportedException("Boss reward has no supported goods: "+reward);
        Entry("ItemLotParam").Bytes=ImportParam.Write(lots);Entry("EquipParamGoods").Bytes=ImportParam.Write(goods);Entry("NpcParam").Bytes=ImportParam.Write(npcs);
        var resultBytes=game.Write(DCX.Type.DCX_DFLT_10000_44_9);files.Add("param/gameparam/gameparam.parambnd.dcx",resultBytes);
        if(questItems.Count>0) {
            var messages=new Dictionary<string,BND3>(StringComparer.Ordinal);
            BND3? Messages(string language) {
                if(messages.TryGetValue(language,out var cached))return cached;
                var bytes=sourceRead($"msg/{language}/item.msgbnd.dcx");if(bytes==null)return null;
                var binder=BND3.Read(bytes);messages.Add(language,binder);return binder;
            }
            var english=Messages("ENGLISH")??throw new InvalidDataException("Source quest-item names unavailable");
            foreach(var directory in Directory.EnumerateDirectories(Path.Combine(nativeRoot,"msg"))) {
                string language=Path.GetFileName(directory),path=$"msg/{language}/item.msgbnd.dcx";var bytes=nativeRead(path);if(bytes==null)continue;
                var native=BND4.Read(bytes);var locale=MapAreaName.SourceLanguage(language,"DS1R");var localized=locale==null?null:Messages(locale);
                foreach(int id in new[]{10,20,24}) {
                    var file=native.Files.Single(f=>f.ID==id);var target=FMG.Read(file.Bytes);
                    var sourceFmg=FMG.Read((localized??english).Files.Single(f=>f.ID==id).Bytes);var fallback=FMG.Read(english.Files.Single(f=>f.ID==id).Bytes);
                    foreach(var (sourceID,targetID) in questItems) {
                        var text=sourceFmg.Entries.SingleOrDefault(e=>e.ID==sourceID)?.Text??fallback.Entries.SingleOrDefault(e=>e.ID==sourceID)?.Text??"";
                        if(id==10&&string.IsNullOrWhiteSpace(text))throw new InvalidDataException("Source quest item has no name: "+sourceID);
                        var existing=target.Entries.SingleOrDefault(e=>e.ID==targetID);
                        if(existing==null)target.Entries.Add(new(target,targetID,text));else if(existing.Text!=text)throw new InvalidDataException("Imported quest-item message conflicts: "+targetID);
                    }
                    file.Bytes=target.Write();
                }
                files.Add(path,native.Write(native.Compression));
            }
        }
        return new(resultBytes,files,reports,pickups,skipped,questItems,vialLot){PickupCarrier=carrierReport,Limits=[
            "Pickup actors contain no native corpse render meshes or rigid bodies; source corpse geometry remains independent. Native attachment dummy90, actor scale and treasure interaction records are retained.",
            "Source DummyObject treasure carriers remain visually absent; their unported source activation scripts are not inferred from the existence of a treasure event. Pickup collection and native glow after visual suppression require gameplay validation."]};
    }
}
