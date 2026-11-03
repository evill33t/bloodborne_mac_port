// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class HydraParameters {
    internal const int Npc=883000,Think=883000,Goal=883000,Variation=88300,HeadNpc=883001,HeadThink=883001;
    internal static readonly IReadOnlyDictionary<int,int> RagdollIds=Enumerable.Range(1400,7).ToDictionary(id=>id,id=>883100+id-1400);
    internal static readonly IReadOnlyDictionary<int,int> EffectIds=Enumerable.Range(5430,7).ToDictionary(id=>id,id=>883200+id-5430)
        .Concat(new Dictionary<int,int>{{7005,883207},{5300,883208},{5151,883209},{90101,883210},{80000,883211},{71100,883212},{7405,883213}}).ToDictionary(p=>p.Key,p=>p.Value);
    internal static readonly IReadOnlyDictionary<int,int> EffectVisualMap=new Dictionary<int,int>{{13530,625306},{13533,625307},{13537,625307}};
    internal static readonly int[] RequiredNativeSfx=[625306,625307];
    internal sealed record Ids(int Npc,int Think,int Goal,int Variation,IReadOnlyDictionary<int,int> Behaviors,IReadOnlyDictionary<int,int> Attacks,
        IReadOnlyDictionary<int,int> Bullets,IReadOnlyDictionary<int,int> Ragdolls,IReadOnlyDictionary<int,int> Effects);
    internal sealed record Report(Ids Ids,IReadOnlyList<int> Judges,IReadOnlyList<int> ProjectileRoots,IReadOnlyList<string> Limitations);
    internal sealed record Result(byte[] Game,Report Report) {internal Ids Ids=>Report.Ids;}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Hydra parameters: "+why);}
    internal static Result Build(byte[] gameBytes,Func<string,byte[]?> source,string definitions,SourceCharacterActions actions,IReadOnlySet<int> sockets) {
        var game=BND4.Read(gameBytes);var original=BND3.Read(source("param/GameParam/GameParam.parambnd.dcx")??throw new FileNotFoundException("Source Hydra parameters unavailable"));
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
        var sourceBehavior=Source("BehaviorParam");var nativeBehavior=Native("BehaviorParam");var sourceAtk=Source("AtkParam_Npc");var nativeAtk=Native("AtkParam_Npc");var sourceBullets=Source("Bullet");var nativeBullets=Native("Bullet");
        int I(PARAM.Row r,string f)=>Convert.ToInt32(r[f].Value);
        var judges=Enumerable.Range(3000,9).SelectMany(id=>actions.Resolve(id).Events).Where(e=>e.Type is 1 or 2).Select(e=>BitConverter.ToInt32(e.GetParameterBytes(false),8)).Where(id=>id!=593).Distinct().Order().ToArray();
        var behaviorRows=judges.ToDictionary(j=>j,j=>sourceBehavior.Rows.Single(r=>I(r,"variationId")==35300&&I(r,"behaviorJudgeId")==j));
        var bulletRoots=behaviorRows.Values.Where(r=>I(r,"refType")==1).Select(r=>I(r,"refId")).Distinct().Order().ToArray();var bullets=new HashSet<int>();var pending=new Queue<int>(bulletRoots);
        while(pending.TryDequeue(out int id)){if(!bullets.Add(id))continue;Need(bullets.Count<=100,"projectile closure budget");var row=sourceBullets.Rows.Single(r=>r.ID==id);int child=I(row,"HitBulletID");if(child>=0)pending.Enqueue(child);}
        var attackSources=behaviorRows.Values.Where(r=>I(r,"refType")==0).Select(r=>I(r,"refId")).Concat(bullets.Select(id=>I(sourceBullets.Rows.Single(r=>r.ID==id),"atkId_Bullet"))).Where(id=>id>0).Distinct().Order().ToArray();
        Need(bulletRoots.Length==28&&bullets.Count==30&&attackSources.Length<=100,"Basin combat closure changed");
        var attackIds=attackSources.Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>88300000+p.n);var bulletIds=bullets.Order().Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>88300500+p.n);
        var behaviorIds=behaviorRows.Values.ToDictionary(r=>checked((int)r.ID),r=>checked(200000000+Variation*1000+I(r,"behaviorJudgeId")));
        var ids=new Ids(Npc,Think,Goal,Variation,behaviorIds,attackIds,bulletIds,RagdollIds,EffectIds);
        foreach(int id in attackSources) {
            var from=sourceAtk.Rows.Single(r=>r.ID==id);Need(I(from,"throwFlag")==0&&I(from,"throwTypeId")==0,"source paired attack is not supported");
            Need(nativeAtk.Rows.Any(r=>r.ID==id),"native legacy Hydra attack carrier missing: "+id);
            Add("AtkParam_Npc",id,attackIds[id],"Imported Hydra source attack "+id,to=> {
                var names=from.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)&&to.Cells.Any(n=>n.Def.InternalName==c.Def.InternalName&&n.Def.DisplayType==c.Def.DisplayType&&n.Def.BitSize==c.Def.BitSize&&n.Def.ArrayLength==c.Def.ArrayLength)).Select(c=>c.Def.InternalName).ToArray();Copy(from,to,names);
                // Verified native legacy Hydra rows use Physical1 where source
                // uses Standard0 or Push4; preserve source damage/contact shape.
                Need(I(from,"atkAttribute") is 0 or 4,"unverified Hydra resistance category");Set(to,"atkAttribute",1);
                for(int i=0;i<5;i++){Need(I(from,"spEffectId"+i)==0,"unported source attack status effect");Set(to,"spEffectId"+i,-1);}
                for(int i=0;i<4;i++)foreach(int n in new[]{1,2}) {
                    string field=$"hit{i}_DmyPoly{n}";int dummy=I(from,field);bool active=Convert.ToSingle(from[$"hit{i}_Radius"].Value)>0;
                    Need(!active||dummy<0||sockets.Contains(dummy),"missing Hydra contact socket: "+dummy);Set(to,field,active&&dummy>=0?HydraCharacters.Socket(dummy):-1);
                }
            });
        }
        foreach(int id in bullets.Order()) {
            var from=sourceBullets.Rows.Single(r=>r.ID==id);var donor=nativeBullets.Rows.Single(r=>r.ID==id);
            foreach(var cell in from.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal))) {
                var native=donor.Cells.SingleOrDefault(c=>c.Def.InternalName==cell.Def.InternalName);Need(native!=null&&cell.Value.Equals(native.Value),"native legacy projectile differs from source: "+id+"/"+cell.Def.InternalName);
            }
            Add("Bullet",id,bulletIds[id],"Imported Hydra source projectile "+id,to=> {
                int attack=I(from,"atkId_Bullet"),child=I(from,"HitBulletID");Set(to,"atkId_Bullet",attack>0?attackIds[attack]:attack);Set(to,"HitBulletID",child>=0?bulletIds[child]:child);
                foreach(string field in new[]{"sfxId_Bullet","sfxId_Hit","sfxId_Flick"}){int fx=I(from,field);if(fx>0){Need(EffectVisualMap.TryGetValue(fx,out int owned),"unverified Hydra projectile SFX: "+fx);Set(to,field,owned);}}
                foreach(string field in new[]{"spEffectIDForShooter","autoSearchNPCThinkID","spEffectId0","spEffectId1","spEffectId2","spEffectId3","spEffectId4"})Need(I(from,field)<=0,"projectile has unowned effect/AI reference");
            });
        }
        foreach(var (judge,from) in behaviorRows) {
            var donor=nativeBehavior.Rows.Single(r=>r.ID==from.ID);Need(I(donor,"variationId")==35300&&I(donor,"refType")==I(from,"refType")&&I(donor,"refId")==I(from,"refId"),"native legacy behavior differs");
            Add("BehaviorParam",checked((int)from.ID),behaviorIds[(int)from.ID],"Imported Hydra behavior "+judge,to=>{Set(to,"variationId",Variation);Set(to,"refId",I(from,"refType")==0?attackIds[I(from,"refId")]:bulletIds[I(from,"refId")]);});
        }
        var sourceEffects=Source("SpEffectParam");int effectDonor=checked((int)Raw("SpEffectParam").Rows.First().ID);
        foreach(var (id,owned) in EffectIds) {
            var from=sourceEffects.Rows.Single(r=>r.ID==id);
            foreach(string field in new[]{"replaceSpEffectId","cycleOccurrenceSpEffectId","atkOccurrenceSpEffectId"})Need(I(from,field)==-1,"source resident effect needs recursive ownership");
            Add("SpEffectParam",effectDonor,owned,"Imported Hydra source effect "+id,to=> {
                Copy(from,to,from.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)&&c.Def.ArrayLength==1&&to.Cells.Any(n=>n.Def.InternalName==c.Def.InternalName&&n.Def.DisplayType==c.Def.DisplayType&&n.Def.BitSize==c.Def.BitSize&&n.Def.ArrayLength==1)).Select(c=>c.Def.InternalName));
                Need(Convert.ToSingle(to["effectEndurance"].Value)==-1,"resident Hydra effect is not indefinite");
                // Native resident5300 explicitly enables the additional BB
                // self-target gate. DS1R has no corresponding field; leaving
                // the neutral default0 prevents these owned resident effects
                // from applying even when source effectTargetSelf is1.
                Set(to,"effectTargetSelfTarget",1);
                if(id is >=5430 and <=5436)Need(I(to,"addBehaviorJudgeId_condition")==id-5429&&I(to,"addBehaviorJudgeId_add")==0,"head suppression fields changed");
            },neutral:true);
        }
        var sourceRagdoll=Source("RagdollParam");int ragDonor=checked((int)Raw("RagdollParam").Rows.First().ID);
        foreach(var (id,owned) in RagdollIds)Add("RagdollParam",ragDonor,owned,"Imported Hydra head mask "+id,to=>Copy(sourceRagdoll.Rows.Single(r=>r.ID==id),to,sourceRagdoll.Rows.Single(r=>r.ID==id).Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)).Select(c=>c.Def.InternalName)),neutral:true);
        var npc=Source("NpcParam").Rows.Single(r=>r.ID==353001);var brain=Source("NpcThinkParam").Rows.Single(r=>r.ID==353001);
        Add("NpcParam",273101,Npc,"Imported Hydra c3530",to=> {
            Copy(npc,to,new[]{"turnVellocity","hitHeight","hitRadius","hitYOffset","weight","hp","mp","getSoul","physGuardCutRate","magGuardCutRate","fireGuardCutRate","thunGuardCutRate","stamina","staminaRecoverBaseVel","def_phys","def_slash","def_blow","def_thrust","def_mag","def_fire","def_thunder","resist_poison","resist_desease","resist_blood","resist_curse","superArmorDurability","knockbackRate","fallDamageDump","useRagdoll","useRagdollCamHit","disableClothRigidHit","isNoDamageMotion","isUnduration","disableRespawn","isEnableNeckTurn"});
            for(int i=1;i<=8;i++)Copy(npc,to,["partsDamageRate"+i]);Copy(npc,to,["weakPartsDamageRate"]);
            Set(to,"behaviorVariationId",Variation);Set(to,"aiThinkId",Think);Set(to,"teamType",23);Set(to,"npcType",0);Set(to,"normalChangeAnimChrId",-1);
            for(int i=0;i<8;i++){int sourceId=I(npc,"spEffectID"+i);Set(to,"spEffectID"+i,sourceId>0?EffectIds[sourceId]:-1);}
            Set(to,"GameClearSpEffectID",EffectIds[I(npc,"GameClearSpEffectID")]);for(int i=1;i<=6;i++)Set(to,"itemLotId_"+i,-1);
            for(int i=0;i<32;i++)Set(to,"modelDispMask"+i,i<7?1:0);
        });
        Add("NpcThinkParam",273100,Think,"Imported source Basin Hydra AI",to=> {
            Copy(brain,to,brain.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)&&c.Def.ArrayLength==1&&to.Cells.Any(n=>n.Def.InternalName==c.Def.InternalName&&n.Def.DisplayType==c.Def.DisplayType&&n.Def.BitSize==c.Def.BitSize&&n.Def.ArrayLength==1)).Select(c=>c.Def.InternalName));
            Set(to,"logicId",10000);Set(to,"battleGoalID",Goal);
            foreach(string field in new[]{"eyeDist_forDark","eyeDist_forPitchDark"})Set(to,field,brain["eye_dist"].Value);
            foreach(string field in new[]{"battleStartDist_forDark","battleStartDist_forPitchDark"})Set(to,field,brain["BattleStartDist"].Value);
        });
        Add("NpcParam",273101,HeadNpc,"Imported Hydra detached head c3531",to=> {
            Set(to,"behaviorVariationId",Variation);Set(to,"aiThinkId",HeadThink);Set(to,"hp",1);Set(to,"getSoul",0);Set(to,"teamType",0);Set(to,"turnVellocity",0);
            Set(to,"hitHeight",.1f);Set(to,"hitRadius",.1f);Set(to,"hitYOffset",0);Set(to,"weight",100);Set(to,"normalChangeAnimChrId",-1);
            for(int i=0;i<8;i++)Set(to,"spEffectID"+i,-1);Set(to,"GameClearSpEffectID",-1);for(int i=1;i<=6;i++)Set(to,"itemLotId_"+i,-1);
            for(int i=0;i<32;i++)Set(to,"modelDispMask"+i,1);Set(to,"isEnableNeckTurn",0);Set(to,"isNoDamageMotion",1);
            Set(to,"useRagdollCamHit",0);Set(to,"disableRespawn",0);
        });
        Add("NpcThinkParam",273100,HeadThink,"Imported Hydra passive severed head",to=> {
            Set(to,"logicId",11000);Set(to,"battleGoalID",11000);foreach(string field in new[]{"eye_dist","ear_dist","nose_dist","BattleStartDist","eyeDist_forDark","eyeDist_forPitchDark","battleStartDist_forDark","battleStartDist_forPitchDark"})Set(to,field,0);
        });
        foreach(var (name,table) in raw)File(name).Bytes=ImportParam.Write(table);
        return new(game.Write(DCX.Type.DCX_DFLT_10000_44_9),new(ids,judges,bulletRoots,[
            "Owned native legacy Hydra projectile rows preserve every shared source ballistic field. Native starflower fluid FX625306/307 replace source water/splash FX13530/33/37; the complete native effect bank/patch closure must be installed by map integration.",
            "Source base statistics, indefinite resident immunities/scaling, seven head suppression effects and source collision masks use owned rows. Native-only effect fields use neutral native defaults; source-only fields have no automatic numeric substitution.",
            "Only the Basin AI3000..3008 closure is enabled. Source judge593 splash pulses lack a matching behavior row and remain suppressed. Reward/quest persistence and detached heads are encounter responsibilities."]));
    }
}
