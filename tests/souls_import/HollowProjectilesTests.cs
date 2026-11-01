// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
using System.Text.Json;

static class HollowProjectilesTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}throw new Exception(message);}
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static byte[] Data(PARAM.Row row)=>((byte[])row.Cells.Single().Value).ToArray();
    static int Int(byte[] data,int offset)=>BitConverter.ToInt32(data,offset);
    static void Int(byte[] data,int offset,int value)=>BitConverter.GetBytes(value).CopyTo(data,offset);
    static PARAM Rows(BND4 bank,string name) {
        var rows=PARAM.Read(bank.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);
        var def=new PARAMDEF{ParamType=rows.ParamType,DataVersion=rows.ParamdefDataVersion,Unicode=true};
        def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=checked((int)rows.DetectedSize)});rows.ApplyParamdef(def);return rows;
    }
    static byte[] Write(PARAM rows) {
        var bytes=rows.Write();foreach(var pair in rows.Rows.Select((r,i)=>(r,i)))if(pair.r.Name==null)Array.Clear(bytes,64+pair.i*24+16,8);return bytes;
    }
    static void Put(BND4 bank,string name,PARAM rows)=>bank.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes=Write(rows);
    static byte[] Modify(byte[] input,string name,Action<PARAM> change) {
        var bank=BND4.Read(input);var rows=Rows(bank,name);change(rows);Put(bank,name,rows);return bank.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
    public static void Main(string[] args) {
        string root=args[0],definitions=args[1],output=args[2];var originals=new Dictionary<string,byte[]>();
        byte[] Read(string path){string file=Path.Combine(root,path);var bytes=File.ReadAllBytes(file);originals.TryAdd(file,SHA256.HashData(bytes));return bytes;}
        byte[] native=Read("param/gameparam/gameparam.parambnd.dcx"),snapshot=native.ToArray();var source=BND4.Read(native);
        var npcs=Rows(source,"NpcParam");
        HollowProjectiles.Binding[] bindings=[new([960000,960001],96000,960000),new([961200],96120,961200),new([969900],96990,969900)];
        foreach(var binding in bindings)foreach(int id in binding.TargetNpcIDs) {
            Check(npcs.Rows.All(r=>r.ID!=id),"Test-owned NPC reservation is occupied by a native row");
            var donor=npcs.Rows.Single(r=>r.ID==(id==960001?263081:263080));var row=new PARAM.Row(donor){ID=id,Name="Imported hollow projectile fixture "+id};
            row.Cells.Single().Value=Data(donor);npcs.Rows.Add(row);
        }
        Put(source,"NpcParam",npcs);byte[] composed=source.Write(DCX.Type.DCX_DFLT_10000_44_9),composedCopy=composed.ToArray();
        // Independently resolve the installed definitions and compare the named
        // fields with the serialized offsets used by the adapter.
        foreach(var (table,definition) in new[]{("NpcParam","NpcParam"),("Bullet","BulletParam"),("BehaviorParam","BehaviorParam")}) {
            var typed=PARAM.Read(source.Files.Single(f=>Leaf(f.Name)==table+".param").Bytes);typed.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,definition+".xml")));
            var opaque=Rows(source,table);
            int Field(PARAM.Row row,string name)=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName==name).Value);
            if(table=="Bullet") {
                var row=typed.Rows.Single(r=>r.ID==940);var data=Data(opaque.Rows.Single(r=>r.ID==940));
                foreach(var (name,offset) in new[]{("atkId_Bullet",0),("sfxId_Bullet",4),("sfxId_Hit",8),("sfxId_Flick",12),("HitBulletID",104)})Check(Field(row,name)==Int(data,offset),"Native arrow field offset mismatch: "+name);
                Check(Field(row,"numShoot")==BitConverter.ToUInt16(data,128)&&Field(row,"isHitBothTeam")==0,"Native arrow count/team contract changed");
            } else if(table=="BehaviorParam") {
                var row=typed.Rows.Single(r=>r.ID==226300450);var data=Data(opaque.Rows.Single(r=>r.ID==row.ID));
                foreach(var (name,offset) in new[]{("variationId",0),("behaviorJudgeId",4),("refId",12)})Check(Field(row,name)==Int(data,offset),"Native behavior offset mismatch");
                Check(Field(row,"refType")==data[9]&&Field(row,"ezStateBehaviorType_old")==data[8],"Native behavior category offset mismatch");
            } else foreach(int id in bindings.SelectMany(b=>b.TargetNpcIDs))Check(Field(typed.Rows.Single(r=>r.ID==id),"behaviorVariationId")==26300,"Rifle donor prefix changed");
        }
        byte[] result=HollowProjectiles.Apply(composed,bindings);var target=BND4.Read(result);
        Check(composed.SequenceEqual(composedCopy)&&native.SequenceEqual(snapshot),"Projectile adapter mutated a supplied input");
        Check(HollowProjectiles.Apply(composed,[]).SequenceEqual(composed),"Empty projectile binding changed its input");
        Check(source.Files.Count==target.Files.Count,"Projectile adapter changed binder membership");
        foreach(var pair in source.Files.Select((file,index)=>(file,index))) {
            var file=pair.file;var after=target.Files[pair.index];Check(file.ID==after.ID&&file.Name==after.Name&&file.Flags==after.Flags,"An existing parameter binder record changed");
            if(Leaf(file.Name) is not("NpcParam.param" or "BehaviorParam.param" or "Bullet.param"))Check(file.Bytes.Span.SequenceEqual(after.Bytes.Span),"An unrelated native parameter file changed");
        }
        var donorBehavior=Rows(source,"BehaviorParam").Rows.Where(r=>Int(Data(r),0)==26300).ToArray();Check(donorBehavior.Length==72,"Native c2630 behavior inventory changed: "+donorBehavior.Length);
        var afterNpc=Rows(target,"NpcParam");var afterBehavior=Rows(target,"BehaviorParam");var afterAmmo=Rows(target,"Bullet");
        foreach(string name in new[]{"NpcParam","BehaviorParam","Bullet"}) {
            var before=Rows(source,name);var after=Rows(target,name);
            var oldIDs=before.Rows.Select(r=>r.ID).ToHashSet();var retained=after.Rows.Where(r=>oldIDs.Contains(r.ID)).ToArray();
            Check(before.Rows.Count==retained.Length,"Original parameter rows disappeared");
            for(int i=0;i<before.Rows.Count;++i) {
                var old=before.Rows[i];var current=retained[i];Check(old.ID==current.ID&&old.Name==current.Name,"Original row identity/order/null-name changed");
                var expected=Data(old);var binding=bindings.SingleOrDefault(b=>b.TargetNpcIDs.Contains(old.ID));
                if(name=="NpcParam"&&binding!=null)Int(expected,0,binding.VariationID);
                Check(expected.SequenceEqual(Data(current)),"An original opaque row changed outside its owned NPC prefix");
            }
        }
        var nativeArrow=Data(Rows(source,"Bullet").Rows.Single(r=>r.ID==940));
        foreach(var binding in bindings) {
            var expected=nativeArrow.ToArray();Int(expected,0,263080);Check(expected.SequenceEqual(Data(afterAmmo.Rows.Single(r=>r.ID==binding.BulletID))),"Owned arrow changed native fields beyond NPC attack reference");
            foreach(var donor in donorBehavior) {
                expected=Data(donor);int judge=Int(expected,4);Int(expected,0,binding.VariationID);if(judge==450)Int(expected,12,binding.BulletID);
                Check(expected.SequenceEqual(Data(afterBehavior.Rows.Single(r=>r.ID==200000000+binding.VariationID*1000+judge))),"Owned behavior changed its native action contract");
            }
            foreach(int id in binding.TargetNpcIDs)Check(Int(Data(afterNpc.Rows.Single(r=>r.ID==id)),0)==binding.VariationID,"Owned NPC did not use its scoped behavior family");
        }
        Check(afterAmmo.Rows.Count==Rows(source,"Bullet").Rows.Count+3&&afterBehavior.Rows.Count==Rows(source,"BehaviorParam").Rows.Count+216,"Owned projectile clone counts changed");
        Reject(()=>HollowProjectiles.Apply(result,bindings),"Reusing claimed projectile reservations was accepted");
        Reject(()=>HollowProjectiles.Apply(composed,[bindings[0],bindings[0]]),"Duplicate projectile owners were accepted");
        Reject(()=>HollowProjectiles.Apply(composed,[bindings[0] with{VariationID=960000}]),"Non-native behavior category prefix was accepted");
        Reject(()=>HollowProjectiles.Apply(composed,[bindings[0] with{TargetNpcIDs=[263080]}]),"A retail NPC could be retargeted");
        Reject(()=>HollowProjectiles.Apply(Modify(composed,"NpcParam",p=>p.Rows.Single(r=>r.ID==960000).Name="retail"),bindings),"Unowned NPC row was accepted");
        Reject(()=>HollowProjectiles.Apply(Modify(composed,"Bullet",p=>{var row=new PARAM.Row(p.Rows.Single(r=>r.ID==940)){ID=960000};p.Rows.Add(row);}),bindings),"Existing arrow row reservation was ignored");
        Reject(()=>HollowProjectiles.Apply(Modify(composed,"Bullet",p=>{var row=p.Rows.Single(r=>r.ID==940);var bytes=Data(row);Int(bytes,104,960000);row.Cells.Single().Value=bytes;}),bindings),"Bullet forward reference reservation was ignored");
        Reject(()=>HollowProjectiles.Apply(Modify(composed,"BehaviorParam",p=>{var row=p.Rows.Single(r=>r.ID==226300451);var bytes=Data(row);Int(bytes,12,960000);row.Cells.Single().Value=bytes;}),bindings),"Behavior forward reference reservation was ignored");
        Reject(()=>HollowProjectiles.Apply(Modify(composed,"NpcParam",p=>{var row=p.Rows.Single(r=>r.ID==263080);var bytes=Data(row);Int(bytes,0,96000);row.Cells.Single().Value=bytes;}),bindings),"Existing NPC variation reservation was ignored");

        byte[] areaBytes=Read("sfx/frpg_sfxbnd_m24.ffxbnd.dcx"),dlcBytes=Read(HollowProjectiles.EffectsPath);var area=BND4.Read(areaBytes);var dlc=BND4.Read(dlcBytes);
        byte[] merged=HollowProjectiles.Effects(areaBytes,dlcBytes);var effects=BND4.Read(merged);
        Check(dlc.Files.Count==397,"Installed arrow DLC closure inventory changed");
        Check(effects.Files.Select(f=>f.ID).Distinct().Count()==effects.Files.Count,"Effect closure IDs collide");
        foreach(var pair in area.Files.Select((file,index)=>(file,index))) {
            var before=pair.file;var after=effects.Files[pair.index];Check(before.ID==after.ID&&before.Name==after.Name&&before.Flags==after.Flags&&before.Bytes.Span.SequenceEqual(after.Bytes.Span),"An original map effect changed");
        }
        foreach(var file in dlc.Files)Check(effects.Files.Single(f=>Leaf(f.Name)==Leaf(file.Name)).Bytes.Span.SequenceEqual(file.Bytes.Span),"An arrow closure payload changed or is absent");
        Check(HollowProjectiles.Effects(merged,dlcBytes).SequenceEqual(merged),"Composing arrow effects twice changes the bank");
        var malformed=BND4.Read(dlcBytes);malformed.Files.RemoveAll(f=>Leaf(f.Name)=="f000252090.fxr");
        Reject(()=>HollowProjectiles.Effects(areaBytes,malformed.Write()),"Missing arrow SFX was accepted");
        malformed=BND4.Read(areaBytes);var fx=dlc.Files.Single(f=>Leaf(f.Name)=="f000252090.fxr");
        malformed.Files.Add(new BinderFile(fx.Flags,99999,fx.Name,new byte[]{1}));
        Reject(()=>HollowProjectiles.Effects(malformed.Write(),dlcBytes),"Conflicting arrow SFX was accepted");
        foreach(var (file,hash) in originals)Check(SHA256.HashData(File.ReadAllBytes(file)).SequenceEqual(hash),"An original asset changed");
        Directory.CreateDirectory(output);File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{bindings,originalBehaviorCount=donorBehavior.Length,clonedBehaviors=216,clonedArrows=3,requiredEffects=HollowProjectiles.EffectIDs,dlcClosureFiles=dlc.Files.Count,composedEffects=effects.Files.Count,originalsUnchanged=originals.Count,limitations=HollowProjectiles.Limitations},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS: 3 scoped arrows/216 cloned behaviors, 4 owned NPC prefixes, all original opaque rows/order/null names preserved; all397 native DLC effect resources retained; {originals.Count} originals unchanged");
    }
}
