// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class HavelParameters {
    internal const int Character=887000,Think=887000,Tooth=8040000,Shield=19040000,Armor=8800000;
    internal const int ToothVariation=8850,ShieldVariation=8851;
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Havel parameters: "+why);}
    // Serialize only one owned typed row, then append its bytes to the opaque
    // native table. All original row bytes, padding, names and order survive.
    internal static byte[] Build(byte[] gameBytes,Func<string,byte[]?> source,string definitions) {
        var game=BND4.Read(gameBytes);var sourceGame=BND3.Read(source("param/GameParam/GameParam.parambnd.dcx")!);
        PARAMDEF Definition(string game,string name)=>PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../"+game+"/Defs/"+name+".xml")));
        BinderFile File(string name)=>game.Files.Single(f=>Leaf(f.Name)==name+".param");
        var tables=new Dictionary<string,PARAM>();
        PARAM Native(string name) {
            if(tables.TryGetValue(name,out var existing))return existing;var bytes=File(name).Bytes;var header=PARAM.Read(bytes);
            var raw=ImportParam.Read(bytes,header.ParamType,checked((int)header.DetectedSize),header.ParamdefDataVersion);tables.Add(name,raw);return raw;
        }
        PARAM Source(string name,string? def=null){var p=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(Definition("DS1R",def??name));return p;}
        byte[] Typed(string name,int donor,Action<PARAM.Row> edit,string? def=null) {
            var p=PARAM.Read(File(name).Bytes);p.ApplyParamdef(Definition("BB",def??name));var row=new PARAM.Row(p.Rows.Single(r=>r.ID==donor));p.Rows.Clear();p.Rows.Add(row);p.Rows.Add(new PARAM.Row(row){ID=donor+1});
            byte[] Encode(){var bytes=p.Write();var header=PARAM.Read(bytes);return ImportParam.Payload(ImportParam.Read(bytes,header.ParamType,checked((int)header.DetectedSize),header.ParamdefDataVersion).Rows[0]);}
            var before=Encode();edit(row);var after=Encode();var output=ImportParam.Payload(Native(name).Rows.Single(r=>r.ID==donor));
            Need(before.Length==after.Length&&before.Length==output.Length,"typed row ABI size changed: "+name+" "+before.Length+"/"+output.Length);
            // A typed writer may normalize reserved bits. Change only the
            // exact bits changed by our named field edits, preserving opaque
            // donor bytes and rejecting a mismatched edited-field ABI.
            for(int i=0;i<output.Length;i++){int changed=before[i]^after[i];Need(((before[i]^output[i])&changed)==0,"edited native field ABI changed: "+name+" byte"+i);output[i]=(byte)((output[i]&~changed)|(after[i]&changed));}
            return output;
        }
        void Set(PARAM.Row row,string field,object value) {
            var cell=row.Cells.Single(c=>c.Def.InternalName==field);cell.Value=Convert.ChangeType(value,cell.Value.GetType(),System.Globalization.CultureInfo.InvariantCulture);
        }
        void Add(string name,int donor,int id,string label,Action<PARAM.Row> edit,string? def=null) {
            var raw=Native(name);var payload=Typed(name,donor,edit,def);ImportParam.Append(raw,raw.Rows.Single(r=>r.ID==donor),id,label,payload);
        }
        var chara=Source("CharaInitParam").Rows.Single(r=>r.ID==6580);
        var lots=Native("ItemLotParam");var vial=MapItems.LotBytes([(MapItemPolicy.BloodVial,1,100),(0,0,900)]);
        var existingVial=lots.Rows.SingleOrDefault(r=>r.ID==MapItems.VialLot);
        if(existingVial==null)ImportParam.Append(lots,lots.Rows.First(),MapItems.VialLot,"Imported enemy Blood Vial",vial);
        else Need(ImportParam.Payload(existingVial).AsSpan().SequenceEqual(vial),"Blood Vial lot occupied");
        Add("CharaInitParam",7040,Character,"Imported DS1R Havel",r=> {
            foreach(string f in new[]{"soulLv","baseVit","baseStr","baseDex","baseMag","baseFai","npcPlayerSex"})Set(r,f,chara[f].Value);
            Set(r,"baseWil",chara["baseEnd"].Value);Set(r,"baseEnd",0);
            Set(r,"equip_Wep_Right_GenId",Tooth);Set(r,"equip_Wep_Left_GenId",Shield);
            foreach(string f in new[]{"equip_Subwep_Right_GenId","equip_Subwep_Left_GenId"})Set(r,f,-1);
            foreach(var (field,index) in new[]{("equip_Helm",0),("equip_Armer",1),("equip_Gaunt",2),("equip_Leg",3)})Set(r,field,Armor+index*1000);
            foreach(string f in new[]{"bodyScaleHead","bodyScaleBreast","bodyScaleAbdomen","bodyScaleArm","bodyScaleLeg"})Set(r,f,0);
            for(int i=1;i<=10;i++){Set(r,$"item_{i:D2}",-1);Set(r,$"itemNum_{i:D2}",0);}
            for(int i=1;i<=5;i++)Set(r,$"equip_Accessory0{i}",-1);
        });
        Add("NpcParam",7040,Character,"Imported DS1R Havel",r=> {
            Set(r,"aiThinkId",Think);Set(r,"getSoul",3000);Set(r,"disableRespawn",1);
            Set(r,"npcType",2);Set(r,"teamType",23); // Native player NPC, enemy from spawn.
            for(int i=0;i<8;i++)Set(r,"spEffectID"+i,-1);
            for(int i=1;i<=6;i++)Set(r,"itemLotId_"+i,-1);
            Set(r,"itemLotId_1",MapItems.VialLot);
        });
        var sourceThink=Source("NpcThinkParam").Rows.Single(r=>r.ID==6580);
        Add("NpcThinkParam",7040,Think,"Imported DS1R Havel",r=> {
            // Source Havel uses generic logic10000. Native12000 attempts a
            // left-slot weapon switch before combat while slot0 is equipped;
            // Havel has no second shield slot and must not enter that loop.
            Need(Convert.ToInt32(sourceThink["logicId"].Value)==10000,"source Havel logic changed");
            Set(r,"logicId",10000);Set(r,"battleGoalID",Think);
            foreach(string f in new[]{"nearDist","midDist","farDist","outDist","eye_dist","ear_dist","ear_soundcut_dist","nose_dist","maxBackhomeDist","backhomeDist","backhomeBattleDist","BattleStartDist","eye_angX","eye_angY","SightTargetForgetTime"})Set(r,f,sourceThink[f].Value);
            foreach(string f in new[]{"eyeDist_forDark","eyeDist_forPitchDark"})Set(r,f,sourceThink["eye_dist"].Value);
            foreach(string f in new[]{"battleStartDist_forDark","battleStartDist_forPitchDark"})Set(r,f,sourceThink["BattleStartDist"].Value);
        });
        var sourceArmor=Source("EquipParamProtector");
        for(int i=0;i<4;i++) {
            int slot=i;var authored=sourceArmor.Rows.Single(r=>r.ID==440000+i*1000);
            Add("EquipParamProtector",150000+i*1000,Armor+i*1000,"Imported Havel armor "+i,r=> {
                Set(r,"equipModelId",HavelEquipment.ArmorModel);Set(r,"equipModelGender",0);
                foreach(string f in new[]{"defensePhysics","defenseMagic","defenseFire","defenseThunder","defenseSlash","defenseBlow","defenseThrust","resistPoison","resistDisease","resistBlood","resistCurse"})
                    if(authored.Cells.Any(c=>c.Def.InternalName==f)&&r.Cells.Any(c=>c.Def.InternalName==f))Set(r,f,authored[f].Value);
            });
        }
        foreach(var (donor,id,model,variation,physics,stamina,strength) in new[]{(8000000,Tooth,HavelEquipment.ToothModel,ToothVariation,435,112,42),(19000000,Shield,HavelEquipment.ShieldModel,ShieldVariation,105,87,51)}) {
            bool right=id==Tooth;
            Add("EquipParamWeapon",donor,id,right?"Imported Dragon Tooth +5":"Imported Havel Greatshield +5",r=> {
                Set(r,"equipModelId",model);Set(r,"behaviorVariationId",variation);Set(r,"attackBasePhysics",physics);Set(r,"attackBaseStamina",stamina);
                Set(r,"correctStrength",strength);Set(r,"correctAgility",0);Set(r,"correctMagic",0);Set(r,"correctFaith",0);
                Set(r,"wepmotionCategory",right?HavelPlayerAnimations.WeaponBank:20);
                Set(r,"wepmotionOneHandId",HavelPlayerAnimations.OneHandBank);Set(r,"wepmotionBothHandId",HavelPlayerAnimations.BothHandsBank);
                for(int i=0;i<3;i++){Set(r,"spEffectBehaviorId"+i,-1);Set(r,i==0?"residentSpEffectId":"residentSpEffectId"+i,-1);}
                foreach(var cell in r.Cells.Where(c=>c.Def.InternalName.StartsWith("dmypolyId_",StringComparison.Ordinal)))Set(r,cell.Def.InternalName,-1);
                foreach(string form in new[]{"A","B"})Set(r,"dmypolyId_Slot0"+(right?"Right":"Left")+"Form"+form,right?HavelEquipment.RightSocket:HavelEquipment.LeftSocket);
                // No duplicate trick-weapon blades or gun ammunition.
                if(right)Set(r,"wepmotionHangType",0);
            });
            Add("WeaponGenParam",donor,id,"Imported Havel weapon generation "+id,r=> {
                Set(r,"wepParamId",id);for(int i=0;i<5;i++)Set(r,"gemGenId_"+i,-1);
            });
        }
        // Preserve native-only attacks used by unconverted reaction/movement
        // states, under owned behavior identities. Source contact judges below
        // explicitly replace overlaps; source IDs never address retail rows.
        var behaviors=Native("BehaviorParam_PC");var attacks=Native("AtkParam_Pc");
        var sourceBehaviors=Source("BehaviorParam_PC","BehaviorParam");var sourceAttacks=Source("AtkParam_Pc","AtkParam");
        var rawSource=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)=="AtkParam_Pc.param").Bytes);
        var rawDef=new PARAMDEF {ParamType=rawSource.ParamType,DataVersion=rawSource.ParamdefDataVersion};rawDef.Fields.Add(new PARAMDEF.Field(rawDef,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=128});rawSource.ApplyParamdef(rawDef);
        foreach(var (from,to,nativeVariation) in new[]{(3500,ToothVariation,800),(4700,ShieldVariation,1900)}) {
            var authored=sourceBehaviors.Rows.Where(r=>Convert.ToInt32(r["variationId"].Value)==from&&Convert.ToInt32(r["refType"].Value)==0).ToDictionary(r=>Convert.ToInt32(r["behaviorJudgeId"].Value));
            var nativeRows=behaviors.Rows.Where(r=>BitConverter.ToInt32(ImportParam.Payload(r))==nativeVariation).ToArray();
            var judges=nativeRows.Select(r=>BitConverter.ToInt32(ImportParam.Payload(r),4)).Concat(authored.Keys).Distinct().ToArray();
            foreach(int judge in judges) {
                var donor=nativeRows.FirstOrDefault(r=>BitConverter.ToInt32(ImportParam.Payload(r),4)==judge)??nativeRows.FirstOrDefault()??behaviors.Rows.Single(r=>r.ID==100800000);var data=ImportParam.Payload(donor);int attack=to*1000+judge;
                if(authored.TryGetValue(judge,out var original)) {
                    var damage=sourceAttacks.Rows.Single(r=>r.ID==Convert.ToInt32(original["refId"].Value));
                    // Paired throws use a separate attacker/victim adapter.
                    if(Convert.ToInt32(damage["throwFlag"].Value)!=0)continue;
                    var bytes=ImportParam.Payload(attacks.Rows.Single(r=>r.ID==800000));Need(bytes.Length==164,"native player attack layout changed");
                    ImportParam.Payload(rawSource.Rows.Single(r=>r.ID==damage.ID)).AsSpan(0,127).CopyTo(bytes);
                    for(int i=0;i<5;i++){Need(BitConverter.ToInt32(bytes,24+i*4)==0,"unported source attack effect");BitConverter.GetBytes(-1).CopyTo(bytes,24+i*4);}
                    // The weapon supplies native material/resistance identity.
                    // Source255 means inherit, and remains the same sentinel.
                    ImportParam.Append(attacks,attacks.Rows.Single(r=>r.ID==800000),attack,"Imported Havel source attack "+damage.ID,bytes);
                    data=Typed("BehaviorParam_PC",checked((int)donor.ID),r=> {
                        foreach(string f in new[]{"behaviorJudgeId","refType","stamina","mp","category"})Set(r,f,original[f].Value);
                    },"BehaviorParam");
                } else if(data[9]==0) {
                    int originalAttack=BitConverter.ToInt32(data,12);var row=attacks.Rows.Single(r=>r.ID==originalAttack);
                    ImportParam.Append(attacks,row,attack,"Imported Havel native contact "+judge,ImportParam.Payload(row));
                }
                BitConverter.GetBytes(to).CopyTo(data,0);BitConverter.GetBytes(judge).CopyTo(data,4);if(data[9]==0)BitConverter.GetBytes(attack).CopyTo(data,12);
                ImportParam.Append(behaviors,donor,100000000+to*1000+judge,"Imported Havel behavior "+to+"/"+judge,data);
            }
        }
        foreach(var (name,table) in tables)File(name).Bytes=ImportParam.Write(table);
        return game.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
