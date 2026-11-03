// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class CatacombsCreaturesEvents {
    internal const int First=12416100;
    internal static int CasterFlag(int slot)=>slot is >=0 and <6?52411130+slot:throw new ArgumentOutOfRangeException(nameof(slot));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs events: "+why);}
    static byte[] Args(params object[] a){using var s=new MemoryStream();using var w=new BinaryWriter(s);foreach(var v in a){int width=v is byte or sbyte?1:4;while(s.Position%width!=0)w.Write((byte)0);switch(v){case byte n:w.Write(n);break;case sbyte n:w.Write(n);break;case int n:w.Write(n);break;case uint n:w.Write(n);break;case float n:w.Write(n);break;default:throw new ArgumentException("Event argument");}}while(s.Position%4!=0)w.Write((byte)0);return s.ToArray();}
    internal static EMEVD.Instruction I(int bank,int id,params object[] a)=>new(bank,id,Args(a));
    internal static byte[] Apply(byte[] bytes,CatacombsCreaturesSource.Plan plan,IReadOnlyList<MapEnemySpawns.Spawn> spawns) {
        var script=EMEVD.Read(bytes);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=First&&e.ID<First+300),"native VM/event reservation");var ctor=script.Events.Single(e=>e.ID==0);
        var actors=spawns.Where(s=>s.SourceEntityID>=0).ToDictionary(s=>s.SourceEntityID);int next=First;
        void Register(EMEVD.Event e){script.Events.Add(e);ctor.Instructions.Add(I(2000,0,0,(int)e.ID,0));}
        foreach(var cohort in plan.Cohorts) {
            int flag=CasterFlag(cohort.Slot),caster=actors[cohort.Caster.EntityID].EntityID;
            var death=new EMEVD.Event(next++,EMEVD.Event.RestBehaviorType.Restart);
            death.Instructions.AddRange([I(1003,101,(byte)0,(byte)0,(byte)0,flag),I(2004,5,caster,(byte)0),I(2004,1,caster,(byte)0),I(1000,4,(byte)0),I(1014,0),I(4,0,(sbyte)0,caster,(byte)1),I(2003,2,flag,(byte)1)]);Register(death);
            var release=new EMEVD.Event(next++,EMEVD.Event.RestBehaviorType.Restart);release.Instructions.Add(I(3,0,(sbyte)0,(byte)1,(byte)0,flag));
            foreach(var p in cohort.Skeletons){int actor=actors[p.EntityID].EntityID;release.Instructions.AddRange([I(2004,12,actor,(byte)0),I(2004,15,actor,(byte)0)]);}Register(release);
            foreach(var p in cohort.Skeletons) {
                int actor=actors[p.EntityID].EntityID;var e=new EMEVD.Event(next++,EMEVD.Event.RestBehaviorType.Restart);
                e.Instructions.AddRange([
                    I(1003,101,(byte)0,(byte)1,(byte)0,flag),I(2004,12,actor,(byte)1),
                    // Retail BB compares the supplied value against current HP.
                    // Equality reaches the exact immortal clamp without entering
                    // collapse at full HP (native compare5 means value <= HP).
                    I(4,14,(sbyte)1,actor,(byte)0,1),I(3,0,(sbyte)2,(byte)1,(byte)0,flag),I(0,0,(sbyte)-1,(byte)1,(sbyte)1),I(0,0,(sbyte)-1,(byte)1,(sbyte)2),I(0,0,(sbyte)0,(byte)1,(sbyte)-1),
                    I(1003,101,(byte)0,(byte)1,(byte)0,flag),I(2004,1,actor,(byte)0),I(2004,15,actor,(byte)1),
                    I(2003,18,actor,7006,(byte)0,(byte)0,(byte)0),I(1001,0,2.5f),I(2003,18,actor,7004,(byte)1,(byte)0,(byte)0),I(1001,0,2.5f),
                    I(1003,101,(byte)0,(byte)1,(byte)0,flag),I(2004,8,actor,CatacombsCreaturesParameters.Heal,(byte)0),I(2003,18,actor,7007,(byte)0,(byte)0,(byte)0),I(1001,0,3.4333334f),
                    I(2004,15,actor,(byte)0),I(2004,1,actor,(byte)1),I(1000,4,(byte)1),
                    I(1014,0),I(2004,12,actor,(byte)0),I(2004,15,actor,(byte)0),I(2004,1,actor,(byte)1),I(1000,4,(byte)0)
                ]);Register(e);
            }
        }
        foreach(var wake in plan.Wakes) {
            var actor=actors[wake.Entity];var e=new EMEVD.Event(next++,EMEVD.Event.RestBehaviorType.Restart);
            e.Instructions.AddRange([I(4,0,(sbyte)1,actor.EntityID,(byte)1),I(1000,2,(byte)0,(byte)1,(sbyte)1),I(2004,19,actor.EntityID,261091),I(2003,18,actor.EntityID,7004,(byte)1,(byte)0,(byte)0),
                I(3,3,(sbyte)0,(byte)1,10000,actor.EntityID,wake.Radius),I(4,0,(sbyte)2,actor.EntityID,(byte)1),I(1000,2,(byte)0,(byte)1,(sbyte)2),I(2004,19,actor.EntityID,actor.ThinkParamID),I(2003,18,actor.EntityID,7005,(byte)0,(byte)0,(byte)0)]);Register(e);
        }
        Need(next-First==78,"controller population changed");var output=script.Write(DCX.Type.DCX_DFLT_10000_44_9);_=EMEVD.Read(output);return output;
    }
}
