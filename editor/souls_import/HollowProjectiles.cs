// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// Owned hollow rifles can display the shipped Bowblade arrow. This retains
// native NPC damage and native arrow ballistics, rather than emulating DS1 bolts.
internal static class HollowProjectiles {
    internal sealed record Binding(IReadOnlyList<int> TargetNpcIDs,int VariationID,int BulletID);
    internal const string EffectsPath="sfx/frpg_sfxbnd_dlceffects.ffxbnd.dcx";
    internal const int Arrow=940,Damage=263080,Variation=26300,Judge=450;
    internal static readonly int[] EffectIDs=[252090,253090,254090];
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static int Int(byte[] bytes,int offset)=>BitConverter.ToInt32(bytes,offset);
    static void Int(byte[] bytes,int offset,int value)=>BitConverter.GetBytes(value).CopyTo(bytes,offset);
    static byte[] Payload(PARAM.Row row)=>((byte[])row.Cells.Single().Value).ToArray();
    static PARAM Read(Memory<byte> bytes,string type,int size,short version) {
        var rows=PARAM.Read(bytes);
        if(rows.ParamType!=type||rows.DetectedSize!=size||rows.ParamdefDataVersion!=version||rows.BigEndian||rows.Format2D!=PARAM.FormatFlags1.LongDataOffset)
            throw new InvalidDataException("Unsupported hollow projectile parameter schema: "+type);
        var def=new PARAMDEF{ParamType=type,DataVersion=version,Unicode=true};
        def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=size});rows.ApplyParamdef(def);return rows;
    }
    static PARAM.Row Unique(PARAM rows,int id)=>rows.Rows.Count(r=>r.ID==id)==1?rows.Rows.Single(r=>r.ID==id):
        throw new InvalidDataException("Missing or ambiguous hollow projectile parameter: "+id);
    static byte[] Write(PARAM rows) {
        var nulls=rows.Rows.Select((r,i)=>(r.Name,i)).Where(p=>p.Name==null).Select(p=>p.i).ToArray();
        var bytes=rows.Write();foreach(int index in nulls)Array.Clear(bytes,64+index*24+16,8);return bytes;
    }
    static void Append(PARAM rows,PARAM.Row donor,int id,string name,byte[] data) {
        if(rows.Rows.Any(r=>r.ID==id))throw new InvalidDataException("Hollow projectile parameter reservation is occupied: "+id);
        var row=new PARAM.Row(donor){ID=id,Name=name};row.Cells.Single().Value=data;
        int at=rows.Rows.FindIndex(r=>r.ID>id);if(at<0)rows.Rows.Add(row);else rows.Rows.Insert(at,row);
    }

    internal static byte[] Apply(byte[] gameParamBnd,IReadOnlyList<Binding> bindings) {
        if(bindings.Count==0)return gameParamBnd.ToArray();
        if(bindings.Count>100)throw new InvalidDataException("Too many hollow projectile owners");
        var owners=new HashSet<int>();var variations=new HashSet<int>();var bullets=new HashSet<int>();
        foreach(var binding in bindings) {
            // Each existing c96xx model owns its hundred NPC rows and its
            // corresponding behavior prefix; mobile/fixed variants share it.
            if(binding.BulletID is <960000 or >969900||binding.BulletID%100!=0||binding.VariationID!=binding.BulletID/10||
                !variations.Add(binding.VariationID)||!bullets.Add(binding.BulletID)||binding.TargetNpcIDs.Count is <1 or >100||
                binding.TargetNpcIDs.Any(id=>id<binding.BulletID||id>=binding.BulletID+100||!owners.Add(id)))
                throw new InvalidDataException("Invalid or duplicate owned hollow projectile binding");
        }
        var game=BND4.Read(gameParamBnd);
        BinderFile File(string leaf)=>game.Files.Count(f=>Leaf(f.Name)==leaf)==1?game.Files.Single(f=>Leaf(f.Name)==leaf):
            throw new InvalidDataException("Missing or ambiguous hollow projectile parameter file: "+leaf);
        var npcFile=File("NpcParam.param");var behaviorFile=File("BehaviorParam.param");var bulletFile=File("Bullet.param");
        var npcs=Read(npcFile.Bytes,"NPC_PARAM_ST",388,5);var behavior=Read(behaviorFile.Bytes,"BEHAVIOR_PARAM_ST",32,2);
        var ammo=Read(bulletFile.Bytes,"BULLET_PARAM_ST",160,2);var attacks=Read(File("AtkParam_Npc.param").Bytes,"ATK_PARAM_ST",164,1);
        _=Unique(attacks,Damage);
        var arrow=Unique(ammo,Arrow);var arrowData=Payload(arrow);
        if(Int(arrowData,0)!=3200400||!EffectIDs.Select((id,i)=>Int(arrowData,4+i*4)==id).All(v=>v)||
            BitConverter.ToSingle(arrowData,16)!=.6f||BitConverter.ToSingle(arrowData,40)!=30f||Int(arrowData,104)!=-1||BitConverter.ToUInt16(arrowData,128)!=1)
            throw new InvalidDataException("Native Bowblade arrow contract changed");
        var donorRows=behavior.Rows.Where(r=>Int(Payload(r),0)==Variation).ToArray();
        if(donorRows.Length==0||donorRows.Select(r=>r.ID).Distinct().Count()!=donorRows.Length)
            throw new InvalidDataException("Native hollow behavior family is missing or ambiguous");
        foreach(var row in donorRows) {
            var data=Payload(row);int judge=Int(data,4);
            if(judge is <0 or >999||data[8]!=2||row.ID!=200000000+Variation*1000+judge)
                throw new InvalidDataException("Native hollow behavior identity rule changed");
            // Retail retains some unused rows whose attack reference has no
            // destination (for example judge440/Atk263047). Preserve those
            // references verbatim; validate the shot we actually replace.
            if(data[9] is not(0 or 1))throw new InvalidDataException("Unvalidated hollow behavior reference type");
        }
        var shot=donorRows.SingleOrDefault(r=>Int(Payload(r),4)==Judge)??throw new InvalidDataException("Native hollow rifle behavior is missing");
        if(Payload(shot)[9]!=1||Int(Payload(shot),12)!=263050||Int(Payload(Unique(ammo,263050)),0)!=Damage)
            throw new InvalidDataException("Native hollow rifle damage contract changed");
        foreach(var binding in bindings) {
            if(npcs.Rows.Any(r=>Int(Payload(r),0)==binding.VariationID)||
                behavior.Rows.Any(r=>Int(Payload(r),0)==binding.VariationID||r.ID>=200000000+binding.VariationID*1000&&r.ID<200001000+binding.VariationID*1000)||
                ammo.Rows.Any(r=>r.ID==binding.BulletID||Int(Payload(r),104)==binding.BulletID)||
                behavior.Rows.Any(r=>Payload(r)[9]==1&&Int(Payload(r),12)==binding.BulletID))
                throw new InvalidDataException("Hollow projectile row or forward-reference reservation is occupied");
            foreach(int id in binding.TargetNpcIDs) {
                var row=Unique(npcs,id);var data=Payload(row);
                if(row.Name?.StartsWith("Imported hollow ",StringComparison.Ordinal)!=true||Int(data,0)!=Variation||
                    !Enumerable.Range(0,8).Any(i=>Int(data,72+i*4)==5615)||!Enumerable.Range(0,8).Any(i=>Int(data,72+i*4)==5402))
                    throw new InvalidDataException("Projectile owner is not an imported native-rifle hollow: "+id);
                Int(data,0,binding.VariationID);row.Cells.Single().Value=data;
            }
            var projectile=arrowData.ToArray();Int(projectile,0,Damage);
            Append(ammo,arrow,binding.BulletID,"Imported hollow arrow "+binding.VariationID,projectile);
            foreach(var donor in donorRows) {
                var data=Payload(donor);int judge=Int(data,4);Int(data,0,binding.VariationID);
                if(judge==Judge)Int(data,12,binding.BulletID);
                Append(behavior,donor,checked(200000000+binding.VariationID*1000+judge),"Imported hollow behavior "+binding.VariationID+"/"+judge,data);
            }
        }
        npcFile.Bytes=Write(npcs);behaviorFile.Bytes=Write(behavior);bulletFile.Bytes=Write(ammo);
        return game.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }

    // Keep the complete installed DLC FXR/texture/model closure. Merely finding
    // the arrow in that bank does not make it resident for a foreign map.
    internal static byte[] Effects(byte[] composedAreaSfx,byte[] nativeDlcEffects) {
        var target=BND4.Read(composedAreaSfx);var source=BND4.Read(nativeDlcEffects);
        int Category(BinderFile file)=>Leaf(file.Name).Split('.').Last().ToLowerInvariant() switch {
            "fxr"=>0,"tpf"=>100000,"flver"=>200000,"anibnd"=>300000,_=>throw new InvalidDataException("Unsupported arrow SFX resource category")
        };
        if(EffectIDs.Any(id=>source.Files.Count(f=>Leaf(f.Name)=="f"+id.ToString("D9")+".fxr")!=1)||source.Files.Any(f=>f.Bytes.Length==0))
            throw new InvalidDataException("Native arrow SFX closure is incomplete");
        if(target.Files.Select(f=>f.ID).Distinct().Count()!=target.Files.Count||
            target.Files.Select(f=>Leaf(f.Name)).Distinct(StringComparer.OrdinalIgnoreCase).Count()!=target.Files.Count||
            source.Files.Select(f=>Leaf(f.Name)).Distinct(StringComparer.OrdinalIgnoreCase).Count()!=source.Files.Count)
            throw new InvalidDataException("Duplicate arrow SFX resource identity");
        var leaves=target.Files.ToDictionary(f=>Leaf(f.Name),StringComparer.OrdinalIgnoreCase);var ids=target.Files.Select(f=>f.ID).ToHashSet();
        foreach(var file in source.Files) {
            int category=Category(file);
            if(leaves.TryGetValue(Leaf(file.Name),out var prior)) {
                if(!prior.Bytes.Span.SequenceEqual(file.Bytes.Span))throw new InvalidDataException("Conflicting arrow SFX resource: "+Leaf(file.Name));
                continue;
            }
            int id=category;while(ids.Contains(id))++id;
            if(id>=category+100000)throw new InvalidDataException("Arrow SFX binder category is exhausted");
            file.ID=id;target.Files.Add(file);leaves.Add(Leaf(file.Name),file);ids.Add(id);
        }
        return target.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
    internal static readonly string[] Limitations=[
        "Crossbows and bows display native Bowblade arrows with native rifle NPC damage. Arrow speed, range, impact effects and physics are borrowed from Bloodborne; source bolt/bow ballistics and source attack damage are not reproduced.",
        "The arrow DLC effect closure must be composed into the destination area SFX bank. Source muzzle placement and firing timing belong to the separate character/motion adapter; projectile targeting, damage and visibility require native gameplay validation."
    ];
}
