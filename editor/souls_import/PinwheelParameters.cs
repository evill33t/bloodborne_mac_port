// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class PinwheelParameters {
    internal const int Npc=888000,Think=888000,Goal=888000,Variation=88800;
    internal static readonly int[] RequiredNativeSfx=[610101,610102];
    internal sealed record Ids(int Npc,int Think,int Goal,int Variation,IReadOnlyDictionary<int,int> Behaviors,IReadOnlyDictionary<int,int> Attacks,IReadOnlyDictionary<int,int> Bullets,IReadOnlyDictionary<int,int> Effects);
    internal sealed record Report(Ids Ids,int SourceVariation,IReadOnlyList<int> Judges,IReadOnlyList<int> ProjectileRoots,IReadOnlyList<string> Limitations);
    internal sealed record Result(byte[] Game,Report Report) {internal Ids Ids=>Report.Ids;}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Pinwheel parameters: "+why);}
    internal static Result Build(byte[] gameBytes,Func<string,byte[]?> source,string definitions,SourceCharacterActions actions,IReadOnlySet<int> sockets) {
        var game=BND4.Read(gameBytes);var original=BND3.Read(source("param/GameParam/GameParam.parambnd.dcx")??throw new FileNotFoundException("Source Pinwheel parameters unavailable"));
        PARAMDEF Definition(string g,string name)=>PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../"+g+"/Defs/"+name+".xml")));
        BinderFile File(string name)=>game.Files.Single(f=>Leaf(f.Name)==name+".param");
        var raw=new Dictionary<string,PARAM>();var typed=new Dictionary<string,PARAM>();var authored=new Dictionary<string,PARAM>();
        string DefName(string name)=>name switch{"AtkParam_Npc"=>"AtkParam","SpEffectParam"=>"SpEffect","Bullet"=>"BulletParam",_=>name};
        PARAM Raw(string name){if(raw.TryGetValue(name,out var p))return p;var f=File(name);var h=PARAM.Read(f.Bytes);p=ImportParam.Read(f.Bytes,h.ParamType,checked((int)h.DetectedSize),h.ParamdefDataVersion);raw.Add(name,p);return p;}
        PARAM Native(string name){if(typed.TryGetValue(name,out var p))return p;p=PARAM.Read(File(name).Bytes);p.ApplyParamdef(Definition("BB",DefName(name)));typed.Add(name,p);return p;}
        PARAM Source(string name){if(authored.TryGetValue(name,out var p))return p;p=PARAM.Read(original.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(Definition("DS1R",DefName(name)));authored.Add(name,p);return p;}
        void Set(PARAM.Row row,string field,object value){var c=row.Cells.Single(c=>c.Def.InternalName==field);c.Value=Convert.ChangeType(value,c.Value.GetType(),System.Globalization.CultureInfo.InvariantCulture);}
        void Copy(PARAM.Row from,PARAM.Row to,IEnumerable<string> names){foreach(string name in names){var a=from.Cells.Single(c=>c.Def.InternalName==name);var b=to.Cells.Single(c=>c.Def.InternalName==name);Need(a.Def.DisplayType==b.Def.DisplayType&&a.Def.BitSize==b.Def.BitSize&&a.Def.ArrayLength==b.Def.ArrayLength,"typed field ABI: "+name);Set(to,name,a.Value);}}
        byte[] Encode(string name,PARAM.Row row){var p=PARAM.Read(File(name).Bytes);p.ApplyParamdef(Definition("BB",DefName(name)));p.Rows.Clear();p.Rows.Add(row);p.Rows.Add(new PARAM.Row(row){ID=row.ID+1});var b=p.Write();var h=PARAM.Read(b);return ImportParam.Payload(ImportParam.Read(b,h.ParamType,checked((int)h.DetectedSize),h.ParamdefDataVersion).Rows[0]);}
        void Add(string name,int donor,int id,string label,Action<PARAM.Row> edit,bool neutral=false) {
            var table=Raw(name);var donorRaw=table.Rows.Single(r=>r.ID==donor);var row=neutral?new PARAM.Row(id,label,Definition("BB",DefName(name))):new PARAM.Row(Native(name).Rows.Single(r=>r.ID==donor)){ID=id,Name=label};
            if(neutral){edit(row);ImportParam.Append(table,donorRaw,id,label,Encode(name,row));return;}
            var before=Encode(name,row);edit(row);var after=Encode(name,row);var data=ImportParam.Payload(donorRaw);
            Need(before.Length==after.Length&&before.Length==data.Length,"native owned row ABI: "+name);
            for(int i=0;i<data.Length;i++){int mask=before[i]^after[i];Need(((before[i]^data[i])&mask)==0,"opaque native edited-field mismatch: "+name);data[i]=(byte)((data[i]&~mask)|(after[i]&mask));}
            ImportParam.Append(table,donorRaw,id,label,data);
        }
        int I(PARAM.Row r,string f)=>Convert.ToInt32(r[f].Value);
        string[] Shared(PARAM.Row from,PARAM.Row to)=>from.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)&&c.Def.ArrayLength==1&&to.Cells.Any(n=>n.Def.InternalName==c.Def.InternalName&&n.Def.DisplayType==c.Def.DisplayType&&n.Def.BitSize==c.Def.BitSize&&n.Def.ArrayLength==1)).Select(c=>c.Def.InternalName).ToArray();
        var npc=Source("NpcParam").Rows.Single(r=>r.ID==332000);var brain=Source("NpcThinkParam").Rows.Single(r=>r.ID==332000);int sourceVariation=I(npc,"behaviorVariationId");Need(sourceVariation==33200,"actual Pinwheel variation changed");
        var sourceBehavior=Source("BehaviorParam");var sourceAtk=Source("AtkParam_Npc");var sourceBullets=Source("Bullet");
        var judges=new[]{3000,3001,3003,3006,3007}.SelectMany(id=>actions.Resolve(id).Events).Where(e=>e.Type is 1 or 2).Select(e=>BitConverter.ToInt32(e.GetParameterBytes(false),8)).Distinct().Order().ToArray();
        Need(judges.SequenceEqual(new[]{100,110,120,130,140,142}),"Pinwheel attack judges changed");
        var behaviorRows=judges.ToDictionary(j=>j,j=>sourceBehavior.Rows.Single(r=>I(r,"variationId")==sourceVariation&&I(r,"behaviorJudgeId")==j));
        var roots=behaviorRows.Values.Where(r=>I(r,"refType")==1).Select(r=>I(r,"refId")).Distinct().Order().ToArray();
        Need(roots.SequenceEqual(new[]{33200,33201,33202,33203}),"Pinwheel projectile roots changed");var bullets=new HashSet<int>();var pending=new Queue<int>(roots);
        while(pending.TryDequeue(out int id)){if(!bullets.Add(id))continue;Need(bullets.Count<=32,"projectile closure budget");int child=I(sourceBullets.Rows.Single(r=>r.ID==id),"HitBulletID");if(child>=0)pending.Enqueue(child);}
        Need(bullets.Order().SequenceEqual(Enumerable.Range(33200,4)),"Pinwheel projectile closure changed");
        var attackSources=behaviorRows.Values.Where(r=>I(r,"refType")==0).Select(r=>I(r,"refId")).Concat(bullets.Select(id=>I(sourceBullets.Rows.Single(r=>r.ID==id),"atkId_Bullet"))).Where(id=>id>0).Distinct().Order().ToArray();
        var attackIds=attackSources.Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>88800000+p.n);var bulletIds=bullets.Order().Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>88800500+p.n);
        var behaviors=behaviorRows.Values.ToDictionary(r=>checked((int)r.ID),r=>checked(200000000+Variation*1000+I(r,"behaviorJudgeId")));
        var sourceNpcs=Source("NpcParam").Rows.Where(r=>r.ID is 332000 or 332001).OrderBy(r=>r.ID).ToArray();Need(sourceNpcs.Length==2,"source Pinwheel profiles");
        var effectSources=sourceNpcs.SelectMany(n=>Enumerable.Range(0,8).Select(i=>I(n,"spEffectID"+i)).Append(I(n,"GameClearSpEffectID"))).Where(id=>id>0).Distinct().Order().ToArray();
        Need(effectSources.SequenceEqual(new[]{5151,5329,5364,7007,7407,71100,80000,90000}),"Pinwheel resident profile changed");var effects=effectSources.Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>888200+p.n);
        var ids=new Ids(Npc,Think,Goal,Variation,behaviors,attackIds,bulletIds,effects);
        foreach(int id in attackSources) {
            var from=sourceAtk.Rows.Single(r=>r.ID==id);Need(I(from,"throwFlag")==0&&I(from,"throwTypeId")==0,"paired attack unsupported");int donor=Native("AtkParam_Npc").Rows.Any(r=>r.ID==id)?id:271000;
            Add("AtkParam_Npc",donor,attackIds[id],"Imported Pinwheel attack "+id,to=> {
                Copy(from,to,Shared(from,to));Need(I(from,"atkAttribute") is 0 or 4,"unverified Pinwheel attack category");Set(to,"atkAttribute",1);
                for(int i=0;i<5;i++){Need(I(from,"spEffectId"+i)<=0,"unowned Pinwheel attack status");Set(to,"spEffectId"+i,-1);}
                for(int i=0;i<4;i++)foreach(int n in new[]{1,2}) {string field=$"hit{i}_DmyPoly{n}";int dummy=I(from,field);bool active=Convert.ToSingle(from[$"hit{i}_Radius"].Value)>0;
                    Need(!active||dummy<0||sockets.Contains(dummy),"source contact socket missing "+dummy);Set(to,field,active&&dummy>=0?PinwheelCharacters.Socket(dummy):-1);}
            });
        }
        foreach(int id in bullets.Order()) {
            var from=sourceBullets.Rows.Single(r=>r.ID==id);var legacy=Native("Bullet").Rows.Single(r=>r.ID==id);
            foreach(var c in from.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)))Need(legacy[c.Def.InternalName].Value.Equals(c.Value),"native legacy Pinwheel bullet differs "+id+"/"+c.Def.InternalName);
            Add("Bullet",id,bulletIds[id],"Imported Pinwheel fire projectile "+id,to=> {
                int attack=I(from,"atkId_Bullet"),child=I(from,"HitBulletID");Set(to,"atkId_Bullet",attack>0?attackIds[attack]:attack);Set(to,"HitBulletID",child>=0?bulletIds[child]:child);
                foreach(string f in new[]{"sfxId_Bullet","sfxId_Hit","sfxId_Flick"}) {int fx=I(from,f);Need(fx is -1 or 0 or 13322 or 13323 or 13325 or 13326,"unverified Pinwheel projectileFX");if(fx>0)Set(to,f,fx is 13322 or 13323?610101:610102);}
                foreach(string f in new[]{"spEffectIDForShooter","autoSearchNPCThinkID","spEffectId0","spEffectId1","spEffectId2","spEffectId3","spEffectId4"})Need(I(from,f)<=0,"unowned projectile status/AI");
            });
        }
        foreach(var (judge,from) in behaviorRows)Add("BehaviorParam",227100100,behaviors[(int)from.ID],"Imported Pinwheel behavior "+judge,to=> {
            Copy(from,to,Shared(from,to));Set(to,"variationId",Variation);int kind=I(from,"refType");Need(kind is 0 or 1,"unsupported behavior reference");Set(to,"refId",kind==0?attackIds[I(from,"refId")]:bulletIds[I(from,"refId")]);
        });
        int effectDonor=(int)Raw("SpEffectParam").Rows.First().ID;
        foreach(var (id,owned) in effects) {var from=Source("SpEffectParam").Rows.Single(r=>r.ID==id);
            foreach(string f in new[]{"replaceSpEffectId","cycleOccurrenceSpEffectId","atkOccurrenceSpEffectId"})Need(I(from,f)==-1,"resident effect reference requires ownership");
            Add("SpEffectParam",effectDonor,owned,"Imported Pinwheel resident "+id,to=>{Copy(from,to,Shared(from,to));Set(to,"effectTargetSelfTarget",1);},neutral:true);
        }
        foreach(int profile in new[]{0,1}) {
        var actor=sourceNpcs[profile];var thinker=Source("NpcThinkParam").Rows.Single(r=>r.ID==332000+profile);
        Add("NpcParam",273101,Npc+profile,"Imported Pinwheel c3320",to=> {
            Copy(actor,to,new[]{"turnVellocity","hitHeight","hitRadius","hitYOffset","weight","hp","mp","getSoul","physGuardCutRate","magGuardCutRate","fireGuardCutRate","thunGuardCutRate","stamina","staminaRecoverBaseVel","def_phys","def_slash","def_blow","def_thrust","def_mag","def_fire","def_thunder","resist_poison","resist_desease","resist_blood","resist_curse","superArmorDurability","knockbackRate","fallDamageDump","useRagdoll","useRagdollCamHit","disableClothRigidHit","isNoDamageMotion","isUnduration","disableRespawn","isEnableNeckTurn"});
            Set(to,"behaviorVariationId",Variation);Set(to,"aiThinkId",Think+profile);Set(to,"teamType",23);Set(to,"npcType",0);Set(to,"normalChangeAnimChrId",-1);Set(to,"normalChangeTexChrId",-1);
            for(int i=0;i<8;i++)Set(to,"spEffectID"+i,effects.GetValueOrDefault(I(actor,"spEffectID"+i),-1));Set(to,"GameClearSpEffectID",effects[I(actor,"GameClearSpEffectID")]);for(int i=1;i<=6;i++)Set(to,"itemLotId_"+i,-1);
            Set(to,"animIdOffset",profile*1000000);Set(to,"nameId",PinwheelEvents.Name);for(int i=0;i<32;i++)Set(to,"modelDispMask"+i,1);
        });
        Add("NpcThinkParam",273100,Think+profile,"Imported Pinwheel source AI",to=> {
            Copy(thinker,to,Shared(thinker,to));Need(I(thinker,"logicId")==10000&&I(thinker,"battleGoalID")==332000+profile,"source logic routing changed");Set(to,"logicId",10000);Set(to,"battleGoalID",Goal+profile);
            foreach(string f in new[]{"eyeDist_forDark","eyeDist_forPitchDark"})Set(to,f,thinker["eye_dist"].Value);foreach(string f in new[]{"battleStartDist_forDark","battleStartDist_forPitchDark"})Set(to,f,thinker["BattleStartDist"].Value);
        });
        }
        Add("SpEffectParam",5625,888299,"Imported Pinwheel illusion heal",to=>{Set(to,"changeHpRate",-100f);Set(to,"effectTargetSelfTarget",1);});
        foreach(var (lot,good,flag) in new[]{(2550,2607,50001550),(9000,500,-1),(9030,330,-1)}) {
            var sourceReward=Source("ItemLotParam").Rows.Single(r=>r.ID==lot);
            Need(I(sourceReward,"getItemFlagId")==flag,"source common970 collection flag changed");
            for(int i=1;i<=8;i++){string suffix=i.ToString("D2");Need(I(sourceReward,"getItemFlagId"+suffix)==0&&I(sourceReward,"lotItemBasePoint"+suffix)==(i==1?100:0),"source common970 outcome conditions changed");
                if(i==1)Need(I(sourceReward,"lotItemId"+suffix)==good&&I(sourceReward,"lotItemCategory"+suffix)==MapItemPolicy.SourceGoodsCategory&&I(sourceReward,"lotItemNum"+suffix)==1,"source common970 item identity changed");}
        }
        Need(I(Source("EquipParamGoods").Rows.Single(r=>r.ID==2607),"goodsType")==1,"source Rite of Kindling quest identity changed");
        var reward=Source("GameAreaParam").Rows.Single(r=>r.ID==1300800);Need(I(reward,"bonusSoul_single")==15000&&I(reward,"bonusSoul_multi")==15000,"source victory reward changed");
        Add("GameAreaParam",(int)Raw("GameAreaParam").Rows.First().ID,Pinwheel.Body,"Imported Pinwheel boss clear",to=>{Set(to,"bonusSoul_single",15000u);Set(to,"bonusSoul_multi",15000u);Set(to,"humanityPointCountFlagIdTop",-1);});
        foreach(var (name,table) in raw)File(name).Bytes=ImportParam.Write(table);
        return new(game.Write(DCX.Type.DCX_DFLT_10000_44_9),new(ids,sourceVariation,judges,roots,[
            "Source Pinwheel variation33200 owns four source fire projectiles and the authored area contacts. Native priest fire visuals replace incompatible source FXR1; ballistics and damage remain source-authored.",
            "All source resident stats and damage-motion immunities are private rows, with BB's explicit resident self-target gate. Pinwheel encounter events own summon/warp/phase/defeat state. Native gameplay remains a separate acceptance check."]));
    }
    internal static IReadOnlyDictionary<string,byte[]> Names(Func<string,byte[]?> source,Func<string,byte[]> read,string nativeRoot) {
        string? Name(string language){var bytes=source("msg/"+language+"/item.msgbnd.dcx");if(bytes==null)return null;return FMG.Read(BND3.Read(bytes).Files.Single(f=>f.ID==18).Bytes).Entries.SingleOrDefault(e=>e.ID==3320)?.Text;}
        string english=Name("ENGLISH")??throw new InvalidDataException("Source Pinwheel name missing");var files=new Dictionary<string,byte[]>();
        foreach(string directory in Directory.EnumerateDirectories(Path.Combine(nativeRoot,"msg"))) {
            if(!System.IO.File.Exists(Path.Combine(directory,"item.msgbnd.dcx")))continue;string language=Path.GetFileName(directory),path="msg/"+language+"/item.msgbnd.dcx";
            var bank=BND4.Read(read(path));var file=bank.Files.Single(f=>f.ID==18);var names=FMG.Read(file.Bytes);Need(!names.Entries.Any(e=>e.ID==PinwheelEvents.Name),"owned boss name occupied");
            names.Entries.Add(new(names,PinwheelEvents.Name,Name(MapAreaName.SourceLanguage(language,"DS1R")??"ENGLISH")??english));file.Bytes=names.Write();files.Add(path,bank.Write(bank.Compression));
        }
        Need(files.Count>0,"no installed native message languages");return files;
    }
}
