// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// Source loadout/brain identities remain explicit. Native combat controllers
// supply the selected role; their movesets and projectiles are approximations.
internal static class HollowCombatProfiles {
    internal enum Kind { Sword,Torch,Axe,SwordShield,SpearShield,Crossbow,CrossbowStationary,Firebomb,RapierShield,Bow }
    internal sealed record Profile(string SourceModel,int SourceNpc,int SourceThink,Kind Role,int NativeNpc,int NativeThink,
        int MotionBank,IReadOnlyList<int> SourceMasks,bool HasShield,bool RangedOnly) {
        internal bool Ranged=>Role is Kind.Crossbow or Kind.CrossbowStationary or Kind.Bow;
        internal int NativeBattleGoal=>Ranged?263030:NativeThink;
        internal int AnimationOffset=>checked(MotionBank*1000000);
        internal int SourceProjectileDummy=>Role==Kind.Bow?30:Ranged?20:Role==Kind.Firebomb?1:-1;
        internal int NativeProjectileDummy=>Ranged?ShotDummy:Role==Kind.Firebomb?1:-1;
        internal IReadOnlyList<int> RequiredEffects=>RangedOnly?[5400+MotionBank,5615,5619]:Ranged?[5400+MotionBank,5615]:[5400+MotionBank];
    }
    internal const string NativeModel="c2630",NativeMap="m24_01_00_01",NativeAi="m27_00_00_00";
    internal const int NativeBones=84,NativeClips=567,NativeLogic=10000,NativeBehaviorVariation=26300;
    internal const int ShotDummy=60,ShotJudge=450;
    internal static Profile? Select(MSB1.Part.Enemy actor,IReadOnlySet<int> masks) {
        if(actor.TalkID>0||actor.CharaInitID>=0||actor.InitAnimID!=-1)return null;
        (Kind Role,int Npc,int Think,int Bank,int[] Masks,bool Shield,bool Fixed)? role=(actor.ModelName,actor.NPCParamID,actor.ThinkParamID) switch {
            ("c2500",250000,250000 or 250001 or 250002) or ("c2500",250050,250007 or 250008)=>
                (Kind.Sword,263030,263030,2,[0],false,false),
            ("c2500",250001,250012 or 250013)=>(Kind.Torch,263000,263000,0,[1],false,false),
            ("c2500",250052,250020)=>(Kind.Bow,263081,263081,2,[2],false,true),
            ("c2540",254000,254008 or 254011) or ("c2540",254010,254000 or 254006 or 254020 or 254030)=>
                (Kind.SwordShield,263020,263020,2,[0,2],true,false),
            ("c2540",254001,254009) or ("c2540",254011,254001 or 254007)=>
                (Kind.Axe,263040,263040,2,[1],false,false),
            ("c2540",254002,254010) or ("c2540",254012,254002)=>
                (Kind.Firebomb,263060,263060,2,[0],false,false),
            ("c2550",255000 or 255040,255000 or 255011)=>
                (Kind.SpearShield,263020,263020,2,[0,1],true,false),
            ("c2550",255001,255001 or 255005 or 255020) or ("c2550",255010,255008) or
                ("c2550",255041,255005 or 255020 or 255040)=>
                (Kind.SwordShield,263020,263020,2,[1,2],true,false),
            ("c2550",255002 or 255042,255002)=>(Kind.Crossbow,263080,263080,2,[3],false,false),
            ("c2550",255002,255007 or 255030) or ("c2550",255042,255004)=>
                (Kind.CrossbowStationary,263081,263081,2,[3],false,true),
            ("c2560",256000,256000 or 256003 or 256008)=>
                (Kind.SwordShield,263020,263020,2,[0,3,7,10],true,false),
            ("c2560",256002,256002 or 256007)=>(Kind.RapierShield,263020,263020,2,[2,4,7,10],true,false),
            _=>null
        };
        if(role is not {} r)return null;
        if(!masks.SetEquals(r.Masks))throw new InvalidDataException("Source hollow loadout masks changed: "+actor.ModelName+"/"+actor.NPCParamID);
        return new(actor.ModelName,actor.NPCParamID,actor.ThinkParamID,r.Role,r.Npc,r.Think,r.Bank,Array.AsReadOnly(r.Masks),r.Shield,r.Fixed);
    }
    internal static byte[] Parameters(Profile profile,byte[] donor,PARAM.Row source,string sourceDefinitions) {
        var native=PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(sourceDefinitions,"../../BB/Defs/NpcParam.xml")));
        int animationField=native.Fields.FindIndex(f=>f.InternalName=="normalChangeAnimChrId");
        if(animationField<0)throw new InvalidDataException("Native hollow animation-owner field is missing");
        var animationPrefix=new PARAMDEF();animationPrefix.Fields.AddRange(native.Fields.Take(animationField));
        var owner=native.Fields[animationField];
        if(native.ParamType!="NPC_PARAM_ST"||native.GetRowSize()!=388||animationPrefix.GetRowSize()!=0x162||
            owner.DisplayType!=PARAMDEF.DefType.s16||owner.ArrayLength!=1||owner.BitSize!=-1)
            throw new InvalidDataException("Native hollow animation-owner parameter layout changed");
        string[] prefix=["behaviorVariationId","aiThinkId","nameId","turnVellocity","hitHeight","hitRadius","weight","hitYOffset"];
        var types=new[]{PARAMDEF.DefType.s32,PARAMDEF.DefType.s32,PARAMDEF.DefType.s32,PARAMDEF.DefType.f32,PARAMDEF.DefType.f32,PARAMDEF.DefType.f32,PARAMDEF.DefType.u32,PARAMDEF.DefType.f32};
        if(source.ID!=profile.SourceNpc||donor.Length!=388||BitConverter.ToInt32(donor,0)!=NativeBehaviorVariation||
            !native.Fields.Take(8).Select(f=>f.InternalName).SequenceEqual(prefix)||!native.Fields.Take(8).Select(f=>f.DisplayType).SequenceEqual(types)||
            !source.Cells.Take(8).Select(c=>c.Def.InternalName).SequenceEqual(prefix)||!source.Cells.Take(8).Select(c=>c.Def.DisplayType).SequenceEqual(types))
            throw new InvalidDataException("Source/native hollow capsule parameter layouts do not match");
        var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(source.Cells.Single(c=>c.Def.InternalName=="modelDispMask"+i).Value)!=0).ToHashSet();
        if(!masks.SetEquals(profile.SourceMasks))throw new InvalidDataException("Source hollow capsule row has another loadout");
        // Keep the native animation-set and gun/turret switches. Npc263080
        // still uses battle263030; effect5615 selects its real rifle branch.
        var effects=Enumerable.Range(0,8).Select(i=>BitConverter.ToInt32(donor,72+i*4)).ToHashSet();
        if(profile.RequiredEffects.Any(e=>!effects.Contains(e)))throw new InvalidDataException("Native hollow role effect is missing");
        if(BitConverter.ToInt16(donor,0x162)!=2630)throw new InvalidDataException("Native hollow animation-bank override changed");
        var bytes=donor.ToArray();
        foreach(var (field,offset,min,max) in new[]{("hitHeight",16,.1f,20f),("hitRadius",20,.05f,10f),("hitYOffset",28,-10f,10f)}) {
            float value=Convert.ToSingle(source.Cells.Single(c=>c.Def.InternalName==field).Value);
            if(!float.IsFinite(value)||value<min||value>max)throw new InvalidDataException("Source hollow capsule is outside the supported range");
            BitConverter.GetBytes(value).CopyTo(bytes,offset);
        }
        // Retail c2630 NPCs explicitly borrow c2630's ANIBND/HKS. Owned
        // wrappers have an appended visible rig in their own bank, so clear
        // this override only in the newly cloned parameter payload.
        BitConverter.GetBytes((short)-1).CopyTo(bytes,0x162);
        return bytes;
    }
    internal static readonly string[] Limitations=[
        "Native c2630 controllers preserve separate melee, shield, firebomb and rifle roles. Source patrols, ambush conditions and complete source AI scripts are not translated.",
        "Spear and rapier roles borrow the native poker-and-shield controller; axe borrows the native cleaver controller. Source visible attacks require the separate authored-motion adapter. Native defense, reactions and reach remain approximate.",
        "Crossbows and bows borrow native rifle shots and their gun/turret state effects. Native projectile appearance, damage and ballistics remain in use until an explicit source projectile adapter is applied."
    ];
}
