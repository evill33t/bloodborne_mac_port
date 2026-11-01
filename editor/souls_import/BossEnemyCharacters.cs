// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// These two authored DS1R actors keep their complete source skin and animation
// rig. Native controllers, original body tracks, action timings and hitboxes
// remain borrowed; this is deliberately not a DS1 combat-script translator.
internal static class BossEnemyCharacters {
    internal static readonly NonHumanoidEnemies.Profile Taurus=new("c2250","c3130",313000,313090,"m29_21_00_00/m29_21_00_46","m29_00_00_00",125,147);
    internal static readonly NonHumanoidEnemies.Profile Capra=new("c2240","c2730",273100,273100,"m24_00_00_00","m24_00_00_00",110,159);
    internal static readonly NonHumanoidEnemies.Profile Dog=new("c3340","c1140",114000,114099,"m22_00_00_00","m22_00_00_00",73,88);
    internal static readonly NonHumanoidEnemies.Profile GargoylePrimary=new("c5350","c3130",313000,313090,"m29_21_00_00/m29_21_00_46","m29_00_00_00",125,147,"gargoyle-primary");
    internal static readonly NonHumanoidEnemies.Profile GargoyleSecondary=GargoylePrimary with{Variant="gargoyle-secondary"};
    internal const int TaurusWaitingAnimation=7000,TaurusJumpAnimation=7010;
    internal static bool IsBoss(NonHumanoidEnemies.Profile profile)=>profile==Taurus||profile==Capra||IsGargoyle(profile);
    internal static bool IsGargoyle(NonHumanoidEnemies.Profile profile)=>profile==GargoylePrimary||profile==GargoyleSecondary;
    internal static NonHumanoidEnemies.Profile? Select(MSB1.Part.Enemy actor,string sourceMap)=>sourceMap=="m10_01_00_00"&&actor.CharaInitID<0&&actor.TalkID<=0&&actor.InitAnimID==-1?
        (actor.ModelName,actor.NPCParamID,actor.ThinkParamID,actor.EntityID) switch {
            ("c2250",225000,225000,1010700)=>Taurus,
            ("c2240",224000,224001,1010750)=>Capra,
            ("c3340",334003,334000,1010500 or 1010501)=>Dog,
            ("c5350",535000,535000,1010800)=>GargoylePrimary,
            ("c5350",535001,535001,1010801)=>GargoyleSecondary,
            _=>null
        }:null;
    internal static int SourceSlot(NonHumanoidEnemies.Profile profile,int native) {
        if(profile==Dog)return DogSlot(native);
        if(IsGargoyle(profile))return GargoyleSlot(native);
        if(!IsBoss(profile))throw new InvalidDataException("Unknown imported boss character profile");
        if(native<0||native/(1000000)>(profile==Taurus?2:1))throw new InvalidDataException("Unsupported native boss playback bank: "+native);
        int slot=native%1000000;
        if(profile==Taurus)return slot switch {
            0 or 200 or 201 or 202 or 203 or 500 or 602 or 603 or 604 or 605 or 701 or 1500 or 1750 or 2200 or 2250 or >=3000 and <=3013=>slot,
            702 or 703=>701,
            2004 or 2005 or 2006 or 2007 or 2010 or 2011 or 2012 or 2013 or 2020 or 2040 or 2060 or 2400 or >=2410 and <=2418 or 2550 or 2551 or 2552=>2000,
            2205=>2200,2255=>2250,
            3020 or 3300=>3000,3021 or 3301=>3001,3014=>3004,3025=>3005,3026=>3006,3030=>3007,
            TaurusWaitingAnimation=>9001,TaurusJumpAnimation=>9061,
            7001 or 7002 or 7011 or 7012 or 8000 or 8010 or 8020 or 8030 or 8040 or 8200=>0,
            _=>throw new InvalidDataException("Unsupported native Taurus playback slot: "+native)
        };
        return slot switch {
            0 or 200 or 201 or 202 or 203 or 500 or 602 or 603 or 604 or 605 or 701 or 702 or 703 or 1500 or 1750 or 2004 or 2005 or 2006 or 2007 or 2080 or 2081 or 2082 or 2083 or 2200 or 2250 or >=3000 and <=3005=>slot,
            2010 or 2011 or 2012 or 2013 or 2020 or 2040 or 2060=>2004,
            2205=>2202,2255=>2250,
            2400 or 2410 or 2411 or 2412 or 2413 or 2414 or 2415 or 2550 or 2551 or 2552=>2004,
            3006 or 3020 or 3027=>3000,3007 or 3011 or 3021 or 3028=>3001,3008 or 3022 or 3029=>3002,
            3009 or 3023 or 3030 or 3039=>3003,3010 or 3012 or 3025 or 3031 or 3033=>3004,
            3015 or 3026 or 3032 or 3034=>3005,
            >=7000 and <=7008 or >=7010 and <=7013 or 7019 or >=7020 and <=7023=>0,
            _=>throw new InvalidDataException("Unsupported native Capra playback slot: "+native)
        };
    }
    static int GargoyleSlot(int native) {
        if(native<0||native/1000000>2)throw new InvalidDataException("Unsupported native Gargoyle playback bank: "+native);
        int slot=native%1000000;return slot switch {
            0 or 200 or 201 or 202 or 203 or 500 or 602 or 603 or 604 or 605 or 701 or 702 or 703 or 1500 or 1750 or 2004 or 2005 or 2006 or 2007 or 2200 or 2250 or 3000 or 3001 or 3003 or 3004 or 3005 or 3006 or 3008 or 3009 or 3010 or 3011 or 3012 or 3014 or 3020 or 3026 or 7000 or 7001 or 8000=>slot,
            2010 or 2011 or 2012 or 2013 or 2020 or 2040 or 2060 or 2400 or >=2410 and <=2418 or 2550 or 2551 or 2552=>2004,
            2205=>2202,2255=>2250,3002=>3003,3007=>3008,3013=>3014,3021=>3022,3025=>3026,3030=>3029,3300=>3500,3301=>3501,
            7002=>7000,7010 or 7011 or 7012=>7001,8010 or 8020 or 8030 or 8040 or 8200=>0,
            _=>throw new InvalidDataException("Unsupported native Gargoyle playback slot: "+native)
        };
    }
    static int DogSlot(int native) {
        if(native==20000200)return 200; // Native fallback locomotion bank.
        if(native is <0 or >=1000000)throw new InvalidDataException("Unsupported native encounter dog playback bank: "+native);
        return native switch {
            0 or 200 or 201 or 202 or 203 or 500 or 502 or 503 or 602 or 603 or 604 or 605 or 701 or 702 or 703 or 1500 or 1750 or 2000 or 2001 or 2002 or 2004 or 2005 or 2006 or 2007 or 2010 or 2011 or 2012 or 2013 or 2014 or 2020 or 2030 or 2031 or 2032 or 2040 or 2050 or 2051 or 2052 or 2053 or 2200 or 2205 or 2207 or 2209 or 2250 or 3000 or 3001 or 3002 or 3004 or 3005 or 3006 or 3500 or 7000=>native,
            700=>701,2003=>2000,2060 or >=2080 and <=2083=>2004,
            2202 or 2203 or 2204 or 2211=>2200,2255=>2250,
            // Native 2400/2401 are additive: appended tracks are identity.
            // Source 2400 is also additive and is not a complete body pose.
            2400 or 2401=>0,
            >=2600 and <=2603 or >=2610 and <=2613 or >=2620 and <=2623=>2000,
            3003 or 3022=>3002,3007=>3004,3008=>3005,3010 or 3020=>3000,3011 or 3021=>3001,3025=>3005,3501=>3500,
            _=>throw new InvalidDataException("Unsupported native encounter dog playback slot: "+native)
        };
    }
    internal static SkinBake Package(Func<string,byte[]?> readSource,Func<string,byte[]> readNative,NonHumanoidEnemies.Profile profile,string target,IReadOnlySet<int> masks,Dictionary<string,byte[]> files) {
        if(!IsBoss(profile)&&profile!=Dog)throw new InvalidDataException("Unknown imported boss character profile");
        var result=NonHumanoidEnemies.Package(readSource,readNative,profile,target,masks,files,native=>SourceSlot(profile,native),IsGargoyle(profile)?bake=>GargoyleMaterials(readSource,masks,bake):null);
        if(profile==Taurus) {
            string path="chr/"+target+".anibnd.dcx";
            files[path]=BossEntranceMotion.Apply(readSource,files[path],readNative("chr/c3130.anibnd.dcx")).Bytes;
        }
        return result;
    }
    static void GargoyleMaterials(Func<string,byte[]?> read,IReadOnlySet<int> masks,SkinBake bake) {
        var authored=MapCharacters.Authored(read,"c5350");var model=FLVER2.Read(authored.Model);
        string Leaf(string value)=>value.Replace('\\','/').Split('/').Last();
        var definitions=new Dictionary<string,MTD>(StringComparer.OrdinalIgnoreCase);
        foreach(string path in new[]{"mtd/Mtd.mtdbnd.dcx","mtd/MtdPatch.mtdbnd.dcx"}) {
            var bytes=read(path)??throw new FileNotFoundException("Gargoyle material definitions unavailable",path);
            foreach(var file in BND3.Read(bytes).Files)if(model.Materials.Any(m=>Leaf(m.MTD).Equals(Leaf(file.Name),StringComparison.OrdinalIgnoreCase)))definitions[Leaf(file.Name)]=MTD.Read(file.Bytes);
        }
        int next=0;
        foreach(var mesh in model.Meshes) {
            var material=model.Materials[mesh.MaterialIndex];var mask=System.Text.RegularExpressions.Regex.Match(material.Name,@"#(\d+)#");if(mask.Success&&!masks.Contains(int.Parse(mask.Groups[1].Value)))continue;
            if(!definitions.TryGetValue(Leaf(material.MTD),out var definition))throw new InvalidDataException("Gargoyle source material definition missing");
            int mode=Convert.ToInt32(definition.Params.Single(p=>p.Name=="g_BlendMode").Value);
            if(mode is not(0 or 1))throw new InvalidDataException("Unsupported Gargoyle source material blend mode");
            var target=bake.Model.Materials[next++];if(mode==1&&!target.MTD.EndsWith("_e.mtd",StringComparison.Ordinal))target.MTD=target.MTD[..^4]+"_e.mtd";
        }
        if(next!=bake.Model.Materials.Count)throw new InvalidDataException("Gargoyle source material/mask ordering changed");
    }
    internal static byte[] Parameters(byte[] donor,PARAM.Row source,string sourceDefinitions) {
        var bytes=NonHumanoidEnemies.Capsule(donor,source,sourceDefinitions);
        var native=PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(sourceDefinitions,"../../BB/Defs/NpcParam.xml")));
        // Both installed schemas have eight scalar four-byte fields followed
        // by u32 hp. Verify this complete prefix instead of guessing its offset.
        var head=native.Fields.Take(9).ToArray();var cells=source.Cells.Take(9).ToArray();
        if(head.Length!=9||cells.Length!=9||head.Where((f,i)=>f.InternalName!=cells[i].Def.InternalName||f.DisplayType!=cells[i].Def.DisplayType||f.ArrayLength!=1||cells[i].Def.ArrayLength!=1||f.BitSize!=-1||cells[i].Def.BitSize!=-1).Any()||head[8].InternalName!="hp"||head[8].DisplayType!=PARAMDEF.DefType.u32)
            throw new InvalidDataException("Source/native boss HP parameter layouts do not match");
        uint health=Convert.ToUInt32(cells[8].Value);if(health is <1 or >99999)throw new InvalidDataException("Unsupported source encounter HP");
        BitConverter.GetBytes(health).CopyTo(bytes,32);return bytes;
    }
    // The borrowed native level effects also multiply HP. Keep their damage,
    // defense, resistances and all opaque fields, with HP-neutral owned clones.
    internal static byte[] NormalizeHealth(byte[] binder,string sourceDefinitions) {
        string definitions=Path.GetFullPath(Path.Combine(sourceDefinitions,"../../BB/Defs"));
        var npcDefinition=PARAMDEF.XmlDeserialize(Path.Combine(definitions,"NpcParam.xml"));
        var effectDefinition=PARAMDEF.XmlDeserialize(Path.Combine(definitions,"SpEffect.xml"));
        string[] prefix=["behaviorVariationId","aiThinkId","nameId","turnVellocity","hitHeight","hitRadius","weight","hitYOffset","hp","mp","getSoul",
            "itemLotId_1","itemLotId_2","itemLotId_3","itemLotId_4","itemLotId_5","itemLotId_6","humanityLotId",
            "spEffectID0","spEffectID1","spEffectID2","spEffectID3","spEffectID4","spEffectID5","spEffectID6","spEffectID7","GameClearSpEffectID"];
        PARAMDEF.DefType[] types=[PARAMDEF.DefType.s32,PARAMDEF.DefType.s32,PARAMDEF.DefType.s32,PARAMDEF.DefType.f32,PARAMDEF.DefType.f32,PARAMDEF.DefType.f32,PARAMDEF.DefType.u32,PARAMDEF.DefType.f32,PARAMDEF.DefType.u32,PARAMDEF.DefType.u32,PARAMDEF.DefType.u32];
        if(npcDefinition.ParamType!="NPC_PARAM_ST"||effectDefinition.ParamType!="SP_EFFECT_PARAM_ST"||
            npcDefinition.Fields.Count<prefix.Length||npcDefinition.Fields.Take(prefix.Length).Where((f,i)=>f.InternalName!=prefix[i]||f.DisplayType!=(i<types.Length?types[i]:PARAMDEF.DefType.s32)||f.ArrayLength!=1||f.BitSize!=-1).Any())
            throw new InvalidDataException("Unsupported encounter NPC effect-reference schema");
        string[] effectPrefix=["iconId","conditionHp","effectEndurance","motionInterval","maxHpRate"];
        if(effectDefinition.Fields.Count<effectPrefix.Length||effectDefinition.Fields.Take(effectPrefix.Length).Where((f,i)=>f.InternalName!=effectPrefix[i]||f.DisplayType!=(i==0?PARAMDEF.DefType.s32:PARAMDEF.DefType.f32)||f.ArrayLength!=1||f.BitSize!=-1).Any())
            throw new InvalidDataException("Unsupported encounter max-HP effect schema");
        (int Source,int Target,float Rate)[] mappings=[(7008,895710,2.176f),(7013,895711,4.235f),(7023,895712,1.764f),
            (7405,895713,4.534f),(7408,895714,2.496f),(7426,895715,4.462f)];
        var game=BND4.Read(binder);
        string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
        var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var effectFile=game.Files.Single(f=>Leaf(f.Name)=="SpEffectParam.param");
        var npcs=ImportParam.Read(npcFile.Bytes,"NPC_PARAM_ST",388,5);var effects=ImportParam.Read(effectFile.Bytes,"SP_EFFECT_PARAM_ST",528,3);
        // Reserve against references as well as row IDs; never claim a retail
        // forward reference just because its destination row is absent.
        foreach(var m in mappings)foreach(var file in game.Files)if(file.Bytes.Span.IndexOf(BitConverter.GetBytes(m.Target))>=0)
            throw new InvalidDataException("Encounter health-effect reservation is occupied: "+m.Target);
        (int ID,uint Health,int Level,int Clear)[] owned=[(895000,1215u,7013,7408),(895100,980u,7023,7426),(895200,84u,7008,7405),(895300,833u,7013,7408),(895400,833u,7013,7408)];
        var pending=new List<(PARAM.Row Row,byte[] Payload)>();var needed=new HashSet<int>();
        foreach(var actor in owned) {
            var row=npcs.Rows.SingleOrDefault(r=>r.ID==actor.ID)??throw new InvalidDataException("Imported encounter health owner is missing: "+actor.ID);
            var data=ImportParam.Payload(row);
            if(row.Name?.StartsWith("Imported boss encounter actor ",StringComparison.Ordinal)!=true||BitConverter.ToUInt32(data,32)!=actor.Health||BitConverter.ToInt32(data,96)!=actor.Level||BitConverter.ToInt32(data,104)!=actor.Clear)
                throw new InvalidDataException("Imported encounter health contract changed: "+actor.ID);
            for(int offset=72;offset<=104;offset+=4) {
                int id=BitConverter.ToInt32(data,offset);if(id<0)continue;
                var effect=effects.Rows.SingleOrDefault(r=>r.ID==id)??throw new InvalidDataException("Encounter resident effect is missing: "+id);
                float rate=BitConverter.ToSingle(ImportParam.Payload(effect),16);if(rate==1)continue;
                int index=Array.FindIndex(mappings,m=>m.Source==id);
                if(index<0||rate!=mappings[index].Rate)throw new InvalidDataException("Unvalidated encounter max-HP scaling effect: "+id);
                needed.Add(id);BitConverter.GetBytes(mappings[index].Target).CopyTo(data,offset);
            }
            pending.Add((row,data));
        }
        if(needed.Count!=mappings.Length)throw new InvalidDataException("Imported encounter HP-effect coverage changed");
        foreach(var m in mappings) {
            var donor=effects.Rows.Single(r=>r.ID==m.Source);var data=ImportParam.Payload(donor);BitConverter.GetBytes(1f).CopyTo(data,16);
            ImportParam.Append(effects,donor,m.Target,"Imported encounter HP-neutral "+m.Source,data);
        }
        foreach(var p in pending)p.Row.Cells.Single().Value=p.Payload;
        npcFile.Bytes=ImportParam.Write(npcs);effectFile.Bytes=ImportParam.Write(effects);
        return game.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
    internal static readonly string[] Limitations=[
        "Taurus and Capra retain their source mesh, equipment, body proportions and authored visible animation tracks. Native donor controllers, body tracks, combat locomotion/extracted motion, action timing, hitboxes, damage and effects remain in use; borrowed attacks are not source-exact movesets.",
        "Taurus slots7000/7010 retain their source waiting/jump poses. The waiting loop is stationary; the jump uses the checked source extracted trajectory, source4.1667-second timing and source movement-state flags. Native controller playback/collision and landing require gameplay validation. Source entrance sound/SFX events are not converted; the encounter adapter owns fog admission, placement and AI activation.",
        "Capra's two swords and source attack poses are visible, but independent left/right source weapon hitboxes are not converted. Bosses and the two source dogs use the existing native material approximation; source shaders and secondary physics are not reproduced. Gargoyle source alpha-tested wings retain native alpha testing.",
        "The two Gargoyles retain their different source equipment masks, wings, tails and authored visible poses. Native ground-combat controllers do not reproduce source flight, fire-breath projectiles or tail severing; phase activation, initial health and persistence belong to the encounter adapter.",
        "Native additive overlays retain their original controller tracks while appended source tracks stay neutral; source additive look/turn overlays are not converted. Extra native dog reaction states reuse a complete authored source hit pose."
    ];
}
