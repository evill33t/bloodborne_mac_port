// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Text.RegularExpressions;

// Source visible motion on an appended rig; native controllers, attack shapes,
// damage and projectiles remain native. Times follow the source pose after its
// normalized sampling onto the unchanged native clip duration.
internal static class HollowCombatMotion {
    internal sealed record Mapping(int NativeID,int SourceID,int HitCarrier=-1,int BulletCarrier=-1,bool Combat=true);
    internal sealed record Action(int ID,int MotionID,string Filename,SourceAnimation Clip,IReadOnlyList<TAE.Event> Events);
    internal sealed record EventBinding(int NativeID,int SourceID,int MotionID,float SourceDuration,float NativeDuration,int Hits,int Shots,int Gates);
    internal sealed record Report(string Model,string Role,IReadOnlyList<EventBinding> Actions,IReadOnlyList<string> Limits);
    static readonly HashSet<int> Gates=[23,78,79,86];
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Require(bool b,string why){if(!b)throw new InvalidDataException("Hollow combat motion: "+why);}
    static int Jump(TAE.Event e){var b=e.GetParameterBytes(false);return e.Type==0&&b.Length>=4?BitConverter.ToInt32(b): -1;}
    internal sealed class Source {
        readonly Dictionary<int,TAE.Animation> animations;
        readonly Dictionary<string,byte[]> files;
        readonly Dictionary<int,Action> cache=[];
        readonly byte[] skeleton;
        internal string Model {get;}
        internal IReadOnlySet<int> IDs {get;}
        internal Source(Func<string,byte[]?> read,string model) {
            Require(model is "c2500" or "c2540" or "c2550" or "c2560","unsupported authored hollow model");Model=model;
            var b=read("chr/"+model+".anibnd.dcx")??throw new FileNotFoundException("Source hollow animation bank unavailable: "+model);
            Require(b.Length is >0 and <=128*1024*1024,"source bank budget exceeded");var bank=BND3.Read(b);
            files=new(StringComparer.OrdinalIgnoreCase);foreach(var f in bank.Files)Require(files.TryAdd(Leaf(f.Name),f.Bytes.ToArray()),"ambiguous source filename");
            skeleton=files.GetValueOrDefault("skeleton.hkx")??throw new InvalidDataException("Source hollow skeleton missing");
            var taes=bank.Files.Where(f=>TAE.Is(f.Bytes.ToArray())).Select(f=>TAE.Read(f.Bytes)).ToArray();
            Require(taes.Length==1&&!taes[0].BigEndian&&taes[0].Format==TAE.TAEFormat.DS1,"unsupported source TAE layout");
            animations=[];foreach(var a in taes[0].Animations)Require(a.ID is >=0 and <100000&&animations.TryAdd((int)a.ID,a),"ambiguous source action ID");IDs=animations.Keys.ToHashSet();
        }
        internal Action Resolve(int id) {
            if(cache.TryGetValue(id,out var result))return result;
            TAE.Animation At(int n,HashSet<int> seen,bool motion) {
                Require(seen.Add(n),"cyclic source animation import");Require(animations.TryGetValue(n,out var a),"missing source action "+n);
                return a!.MiniHeader switch {
                    TAE.Animation.AnimMiniHeader.ImportOtherAnim i=>At(i.ImportFromAnimID,seen,motion),
                    TAE.Animation.AnimMiniHeader.Standard s when motion&&s.ImportsHKX&&s.ImportHKXSourceAnimID>=0=>At(s.ImportHKXSourceAnimID,seen,true),
                    TAE.Animation.AnimMiniHeader.Standard=>a,
                    _=>throw new InvalidDataException("Unsupported source miniheader")
                };
            }
            var events=At(id,[],false);var hkx=At(id,[],true);
            var match=Regex.Match(Leaf(hkx.AnimFileName),@"\Aa(\d{2})_(\d{4})\.(?:hkxwin|hkx)\z",RegexOptions.IgnoreCase);
            Require(match.Success,"unsupported source motion filename");int motionID=int.Parse(match.Groups[1].Value)*10000+int.Parse(match.Groups[2].Value);
            string name=$"a{motionID/10000:D2}_{motionID%10000:D4}.hkx";
            Require(files.TryGetValue(name,out var bytes),"source motion is not local to its declared bank: "+name);
            var clip=SourceAnimation.Decode(skeleton,bytes!);
            result=new(id,motionID,name,clip,events.Events.Select(e=>e.GetClone(false)).ToArray());cache.Add(id,result);return result;
        }
    }
    internal sealed class Plan {
        readonly Source source;readonly Dictionary<int,Mapping> mappings;
        internal string Role {get;}
        internal string Model=>source.Model;
        internal IReadOnlyList<Mapping> Mappings {get;}
        internal IReadOnlyDictionary<int,int> MotionSourceIDs {get;init;}=new Dictionary<int,int>();
        internal IReadOnlySet<int> ReferenceFallbackIDs {get;init;}=new HashSet<int>();
        internal IReadOnlyDictionary<int,int> NativeMotionIDs {get;init;}=new Dictionary<int,int>();
        internal IReadOnlySet<int> SharedNativeActions {get;init;}=new HashSet<int>();
        internal IReadOnlySet<int> UnusedNativeMotionIDs {get;init;}=new HashSet<int>();
        internal Plan(Source from,string role,IReadOnlyList<Mapping> values) {
            source=from;Role=role;Require(values.Count is >0 and <=4096,"invalid mapping budget");mappings=[];
            foreach(var m in values){Require(m.NativeID is >=0 and <14000000&&m.SourceID is >=0 and <100000&&mappings.TryAdd(m.NativeID,m),"invalid/repeated native action mapping");_=source.Resolve(m.SourceID);}Mappings=values.ToArray();
        }
        internal Action Action(int native)=>source.Resolve(mappings.TryGetValue(native,out var m)?m.SourceID:throw new InvalidDataException("Unmapped native hollow action: "+native));
        internal SourceAnimation Clip(int native)=>MotionSourceIDs.TryGetValue(native,out int sourceID)?source.Resolve(sourceID).Clip:Action(native).Clip;
        internal int SourceID(int native)=>Action(native).ID;
        internal int NativeMotionID(int action)=>NativeMotionIDs.TryGetValue(action,out int id)?id:throw new InvalidDataException("Unmapped native motion alias: "+action);
    }
    internal static Plan Load(Func<string,byte[]?> read,string model,string role,IReadOnlyList<Mapping> mappings)=>new(new(read,model),role,mappings);

    internal static Plan Create(Func<string,byte[]?> read,string model,HollowCombatProfiles.Kind role,BND4 nativeBank) {
        var source=new Source(read,model);var taeFiles=nativeBank.Files.Where(f=>TAE.Is(f.Bytes.ToArray())).ToArray();
        Require(taeFiles.Length==1,"native TAE population changed");var tae=TAE.Read(taeFiles[0].Bytes);
        Require(!tae.BigEndian&&tae.Format==TAE.TAEFormat.DS3&&tae.ID==202630&&tae.Animations.Count==688,"native c2630 action population changed");
        var physical=nativeBank.Files.Where(f=>f.ID>=1000000000).Select(f=>f.ID-1000000000).ToHashSet();
        Require(physical.Count==567,"native c2630 motion population changed");
        int offset=(model,role) switch {
            ("c2500",HollowCombatProfiles.Kind.Sword)=>0,("c2500",HollowCombatProfiles.Kind.Torch)=>10000,("c2500",HollowCombatProfiles.Kind.Bow)=>20000,
            ("c2540",HollowCombatProfiles.Kind.SwordShield or HollowCombatProfiles.Kind.Firebomb)=>0,("c2540",HollowCombatProfiles.Kind.Axe)=>10000,
            ("c2550",HollowCombatProfiles.Kind.SpearShield)=>0,("c2550",HollowCombatProfiles.Kind.SwordShield)=>10000,
            ("c2550",HollowCombatProfiles.Kind.Crossbow or HollowCombatProfiles.Kind.CrossbowStationary)=>20000,
            ("c2560",HollowCombatProfiles.Kind.SwordShield)=>0,("c2560",HollowCombatProfiles.Kind.RapierShield)=>20000,
            _=>throw new InvalidDataException("Unsupported source hollow role/model")
        };
        bool ranged=role is HollowCombatProfiles.Kind.Crossbow or HollowCombatProfiles.Kind.CrossbowStationary or HollowCombatProfiles.Kind.Bow;
        int bank=role==HollowCombatProfiles.Kind.Torch?0:2000000;
        // These are native script action families, not coincidentally equal
        // animation numbers. Source banks select the authored equipped style.
        var active=new Dictionary<int,int>();
        void Set(int[] native,int[] from){Require(native.Length==from.Length,"invalid role mapping table");for(int i=0;i<native.Length;i++)active.Add(bank+native[i],from[i]);}
        if(ranged) {
            Set([3030,3031,3032,3036,3039],[offset+3000,offset+3000,offset+3001,offset+3000,offset+3000]);
            if(model=="c2550")Set([3033,3034],[3010,3011]);
            // Mobile native rifle interrupts use its melee hand. The source
            // crossbow model has its own melee channels; equipment visibility
            // during a complete weapon switch is a separate adapter.
            Set([3000,3001,3002,3005],[3000,3001,3002,3003]);
        } else if(role==HollowCombatProfiles.Kind.Torch)Set([3000,3001,3002,3003,3004,3005,3006,3007,3008,3009],[13000,13001,13002,13003,13004,13005,13006,13002,3009,3009]);
        else if(role==HollowCombatProfiles.Kind.Sword)Set([3008,3009,3010,3011,3012,3013,3014,3000,3001,3002,3005],[3000,3001,3002,3003,3008,3004,3007,3000,3001,3002,3003]);
        else if(role==HollowCombatProfiles.Kind.Axe)Set([3023,3024,3025,3026,3027,3028,3029,3020],[13000,13001,13002,13003,13004,13005,13006,13007]);
        else if(role==HollowCombatProfiles.Kind.Firebomb)Set([3021,3022,3037,3038],[3008,3008,3000,3001]);
        else if(model=="c2540")Set([3000,3001,3002,3003,3004],[3000,3001,3002,3008,3007]);
        else if(model=="c2550"&&role==HollowCombatProfiles.Kind.SpearShield)Set([3000,3001,3002,3003,3004],[3000,3001,3002,3008,3006]);
        else if(model=="c2550")Set([3000,3001,3002,3003,3004],[13000,13001,13002,13004,13005]);
        else Set([3000,3001,3002,3003,3004],[offset+3000,offset+3001,offset+3002,offset+3003,offset+3004]);
        int hit=bank+(role switch {HollowCombatProfiles.Kind.Sword=>3008,HollowCombatProfiles.Kind.Axe=>3023,HollowCombatProfiles.Kind.Firebomb=>3037,_=>3000});
        int bullet=bank+(ranged?3031:role==HollowCombatProfiles.Kind.Firebomb?3021:3003);
        int fallbackAttack=offset+3000;var reference=new HashSet<int>();
        int SourceID(int id) {
            if(active.TryGetValue(id,out int chosen))return chosen;int n=id%1000000;
            // Source2300/2330/2360/2400/2430 families are additive overlays,
            // not absolute body poses. NativeRigAppend keeps native additive
            // body tracks and supplies identity for every appended bone.
            if(n is >=2300 and <=2433){reference.Add(id);return 0;}
            if(n is >=3000 and <4000)return source.IDs.Contains(offset+n)?offset+n:source.IDs.Contains(n)?n:fallbackAttack;
            if(source.IDs.Contains(offset+n))return offset+n;if(source.IDs.Contains(n))return n;
            int replacement=n switch {
                2003=>2004,2060=>2014,2110=>2100,2211 or 2220 or 2221=>2200,2261 or 2270 or 2271=>2250,2303=>2302,
                >=2600 and <=2623=>2020,>=2900 and <=2903=>2020,
                _=>-1
            };
            if(replacement>=0){Require(source.IDs.Contains(replacement),"source reaction fallback missing");return replacement;}
            reference.Add(id);return 0;
        }
        var nativeActions=tae.Animations.ToDictionary(a=>checked((int)a.ID));
        int Physical(int id,HashSet<int>? seen=null) {
            seen??=[];Require(seen.Add(id),"cyclic native animation import");Require(nativeActions.TryGetValue(id,out var a),"invalid native animation import");
            if(a!.MiniHeader is TAE.Animation.AnimMiniHeader.ImportOtherAnim i)return Physical(i.ImportFromAnimID,seen);
            if(a.MiniHeader is TAE.Animation.AnimMiniHeader.Standard s&&s.ImportsHKX&&s.ImportHKXSourceAnimID>=0)return Physical(s.ImportHKXSourceAnimID,seen);
            // Retail retains obsolete authoring labels such as a00_12300.
            // The action ID/import fields, not this label, address the HKX.
            return id;
        }
        var shared=tae.Animations.Where(a=>!physical.Contains(Physical(checked((int)a.ID)))).Select(a=>checked((int)a.ID)).ToHashSet();
        Require(shared.Count==31,"native shared motion population changed");
        var choices=tae.Animations.Where(a=>!shared.Contains(checked((int)a.ID))).Select(a=>(Animation:a,ID:checked((int)a.ID),Physical:Physical(checked((int)a.ID)),Source:SourceID(checked((int)a.ID)))).ToArray();
        var motions=new Dictionary<int,int>();var fallback=new HashSet<int>();var final=new Dictionary<int,int>();
        foreach(var group in choices.GroupBy(a=>a.Physical)) {
            var selected=group.Where(a=>active.ContainsKey(a.ID)).ToArray();
            var selectedMotions=selected.Select(a=>source.Resolve(a.Source).MotionID).Distinct().ToArray();
            Require(selectedMotions.Length<=1,"selected role aliases require distinct physical clips: "+group.Key);
            var choice=selected.FirstOrDefault();if(selected.Length==0)choice=group.FirstOrDefault(a=>a.ID==group.Key);if(choice.Animation==null)choice=group.First();
            motions.Add(group.Key,choice.Source);if(reference.Contains(choice.ID))fallback.Add(group.Key);
            foreach(var item in group)final.Add(item.ID,choice.Source);
        }
        var unused=physical.Where(id=>!motions.ContainsKey(id)).ToHashSet();Require(unused.Count==7,"native unused motion population changed");
        foreach(int id in unused){motions.Add(id,SourceID(id));if(reference.Contains(id))fallback.Add(id);}
        Require(motions.Count==567&&physical.SetEquals(motions.Keys),"native physical motion coverage changed");
        var mappings=choices.Select(a=>new Mapping(a.ID,final[a.ID],hit,bullet,a.ID%1000000 is >=3000 and <4000&&a.Animation.Events.Any(e=>e.Type is 1 or 2))).ToArray();
        return new(source,role.ToString(),mappings){MotionSourceIDs=motions,ReferenceFallbackIDs=fallback,NativeMotionIDs=choices.ToDictionary(a=>a.ID,a=>a.Physical),SharedNativeActions=shared,UnusedNativeMotionIDs=unused};
    }

    // Carrier IDs are explicit native TAE action IDs. A single checked native
    // payload supplies native attack/projectile identity; source events supply
    // their multiplicity and exact visible-motion windows. No DS1 judge IDs
    // are copied into Bloodborne's unrelated parameter namespace.
    internal static Report Apply(TAE native,Plan plan,IReadOnlyDictionary<int,float> durations) {
        Require(!native.BigEndian&&native.Format is TAE.TAEFormat.BB or TAE.TAEFormat.DS3,"target must be native Bloodborne TAE");
        var actions=native.Animations.ToDictionary(a=>checked((int)a.ID));var pending=new List<(TAE.Animation Animation,List<TAE.Event> Events,TAE.EventGroup[] Groups)>();var report=new List<EventBinding>();
        foreach(var map in plan.Mappings) {
            Require(actions.TryGetValue(map.NativeID,out var animation),"mapped target action missing");
            Require(durations.TryGetValue(map.NativeID,out float duration)&&float.IsFinite(duration)&&duration is >0 and <=600,"missing native action duration");
            var source=plan.Action(map.NativeID);float scale=duration/source.Clip.Duration;
            var hits=map.Combat?source.Events.Where(e=>e.Type==1).ToArray():[];var shots=map.Combat?source.Events.Where(e=>e.Type==2).ToArray():[];
            var gates=map.Combat?source.Events.Where(e=>Gates.Contains(Jump(e))).ToArray():[];
            TAE.Event Carrier(int type,int id) {
                Require(actions.TryGetValue(id<0?map.NativeID:id,out var a),"native carrier action missing");
                var events=a!.Events.Where(e=>e.Type==type).ToArray();Require(events.Length>0,"native carrier has no required event "+type);
                var e=events[0];Require(e.Group==null||a.EventGroups.Contains(e.Group),"native carrier group is not owned by its action");var b=e.GetParameterBytes(false);Require(type==1?b.Length==16:b.Length is 16 or 24 or 32,"native carrier layout changed");return e;
            }
            var h=hits.Length==0?null:Carrier(1,map.HitCarrier);var s=shots.Length==0?null:Carrier(2,map.BulletCarrier);
            var prototype=animation!.Events.FirstOrDefault(e=>e.Type==0);
            Require(gates.Length==0||prototype!=null,"native continuation gate prototype missing");
            // SoulsFormats.GetClone omits Group. Retained events must keep
            // their original target-owned membership through serialization.
            var output=animation.Events.Where(e=>!map.Combat||(e.Type is not(1 or 2)&&!Gates.Contains(Jump(e)))).Select(e=>{Require(e.Group==null||animation.EventGroups.Contains(e.Group),"native event group is not owned by its action");var copy=e.GetClone(false);copy.Group=e.Group;return copy;}).ToList();
            var foreign=new Dictionary<TAE.EventGroup,TAE.EventGroup>();
            void Add(TAE.Event e,TAE.Event carrier,bool gate) {
                // Authored TAE windows may outlive the sampled clip: source
                // cancel gates and even hit windows retain the held final
                // pose. Preserve those tails instead of silently truncating.
                Require(float.IsFinite(e.StartTime)&&float.IsFinite(e.EndTime)&&e.StartTime>=0&&e.EndTime>=e.StartTime&&e.StartTime<=source.Clip.Duration+.001f&&e.EndTime<=600,"invalid source combat event time: "+source.ID);
                float start=e.StartTime*scale,end=e.EndTime*scale;Require(float.IsFinite(start)&&float.IsFinite(end)&&end<=600,"normalized source event exceeds time budget");
                var bytes=carrier.GetParameterBytes(false).ToArray();var input=e.GetParameterBytes(false);
                Require(gate?input.Length is 12 or 16:e.Type==1?input.Length==12:input.Length==16,"source combat event layout changed");
                if(gate){Require(bytes.Length==16,"native continuation layout changed");Require(bytes.AsSpan(12).IndexOfAnyExcept((byte)0)<0&&(input.Length==12||input.AsSpan(12).IndexOfAnyExcept((byte)0)<0),"continuation extension changed");Array.Copy(input,bytes,12);}
                var group=carrier.Group;if(group!=null&&!animation.EventGroups.Contains(group)){if(!foreign.TryGetValue(group,out var copy)){copy=group.GetClone();foreign.Add(group,copy);}group=copy;}
                output.Add(new(start,end,carrier.Type,carrier.Unk04,bytes,false){Group=group});
            }
            foreach(var e in hits)Add(e,h!,false);foreach(var e in shots)Add(e,s!,false);foreach(var e in gates)Add(e,prototype!,true);
            pending.Add((animation,output,foreign.Values.ToArray()));report.Add(new(map.NativeID,source.ID,source.MotionID,source.Clip.Duration,duration,hits.Length,shots.Length,gates.Length));
        }
        // Commit only after every source motion/carrier/event has validated.
        foreach(var p in pending){p.Animation.EventGroups.AddRange(p.Groups);p.Animation.Events.Clear();p.Animation.Events.AddRange(p.Events);}
        return new(plan.Model,plan.Role,report,["Visible source poses and their attack/projectile/AI-cancel windows are sampled onto native clip timing. Native combat decisions, physical anchors, damage and projectile types remain approximations.","Only explicit mapped actions change. Source sound, effects, special attacks and weapon-transition visibility are separate adapters."]);
    }
}
