// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text;

internal static class HydraAnimations {
    internal sealed record Binding(int NativeAction,int SourceAction,int SourceMotion,string SourceArchive,float Duration,int Contacts,int Projectiles,bool AliasCopy,bool Additive);
    internal sealed record Report(int RetailClips,int AliasCopies,int OwnedActions,int StationaryClips,int SuppressedUnresolvedPulses,IReadOnlyList<Binding> Bindings,IReadOnlyList<string> Limitations);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Hydra animation: "+why);}
    internal static int SourceSlot(SourceCharacterActions source,int native) {
        Need(native/1000000 is 0 or 1,"unexpected native action bank");int action=native%1000000;
        if(action==7005)return 8000;
        return source.Contains(action)?action:action is >=2200 and <2250?2200:action is >=2250 and <2300?2250:0;
    }
    // Keep native compressed controller channels but remove their extracted
    // translation/yaw, and use the actual source duration for the appended pose.
    internal static byte[] Stationary(byte[] bytes,float duration) {
        var edit=new NativeRigAppend.Edit(bytes);var p=edit.Pack;int anim=p.Object("hkaSplineCompressedAnimation"),frames=p.Int(anim+56);
        Need(float.IsFinite(duration)&&duration>0&&duration<=600&&frames>=2,"invalid source timing");
        float dt=duration/(frames-1);edit.Int(anim+16,BitConverter.SingleToInt32Bits(duration));edit.Int(anim+72,BitConverter.SingleToInt32Bits(255*dt));
        edit.Int(anim+76,BitConverter.SingleToInt32Bits(1/(255*dt)));edit.Int(anim+80,BitConverter.SingleToInt32Bits(dt));
        int frame;try{frame=p.Object("hkaDefaultAnimatedReferenceFrame");}catch(InvalidDataException){return edit.Finish();}
        var samples=p.Array(frame+56,16,65536);Need(samples.Count>=2,"empty native extracted motion");edit.Array(frame+56,samples.Count,new byte[samples.Count*16]);edit.Int(frame+48,BitConverter.SingleToInt32Bits(duration));return edit.Finish();
    }
    internal static Report Apply(BND4 bank,NativeRigAppend.Plan plan,SourceCharacterActions source,BND4 bulletCarrier) {
        var entry=bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));var tae=TAE.Read(entry.Bytes);Need(!tae.BigEndian&&tae.Format==TAE.TAEFormat.BB,"native event ABI");
        var old=bank.Files.Where(f=>f.ID>=1000000000).ToDictionary(f=>f.ID-1000000000);Need(old.Count==159,"native c2730 physical population changed");
        var originalRows=tae.Animations.ToDictionary(a=>(int)a.ID,a=>a.MiniHeader.GetClone());
        int Physical(int id,HashSet<int>? seen=null){seen??=[];Need(seen.Count<64&&seen.Add(id),"native import cycle");if(!originalRows.TryGetValue(id,out var header))return old.ContainsKey(id)?id:-1;return header switch {
            TAE.Animation.AnimMiniHeader.ImportOtherAnim i=>Physical(i.ImportFromAnimID,seen),
            TAE.Animation.AnimMiniHeader.Standard s when s.ImportsHKX&&s.ImportHKXSourceAnimID>=0=>Physical(s.ImportHKXSourceAnimID,seen),_=>old.ContainsKey(id)?id:-1};}
        var hit=tae.Animations.SelectMany(a=>a.Events).First(e=>e.Type==1);var gate=tae.Animations.SelectMany(a=>a.Events).First(e=>e.Type==0);
        var shot=TAE.Read(bulletCarrier.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes).Animations.SelectMany(a=>a.Events).First(e=>e.Type==2);
        Need(hit.GetParameterBytes(false).Length==16&&gate.GetParameterBytes(false).Length==16&&shot.GetParameterBytes(false).Length==16&&hit.Group?.GroupType==1&&gate.Group?.GroupType==0&&shot.Group?.GroupType==2,$"native event/group carrier changed: hit{hit.GetParameterBytes(false).Length}/group{hit.Group?.GroupType}, gate{gate.GetParameterBytes(false).Length}/group{gate.Group?.GroupType}, shot{shot.GetParameterBytes(false).Length}/group{shot.Group?.GroupType}");
        Need(Matrix4x4.Invert(plan.Original.Local[0],out var inverse),"singular native root");
        var bindings=new List<Binding>();var generated=new Dictionary<int,byte[]>();int suppressed=0,aliases=0;
        byte[] Motion(int id,byte[] template,out bool additive) {
            var action=source.Resolve(SourceSlot(source,id));var p=new NativeAnimation.Pack(template);int bind=p.Object("hkaAnimationBinding"),anim=p.Object("hkaSplineCompressedAnimation");
            additive=template[p.Data+bind+80]==1;float duration=additive?p.Float(anim+16):action.Clip.Duration;
            var appended=NativeRigAppend.Clip(template,plan,t=>{var pose=action.Clip.Sample(t*action.Clip.Duration);pose[0]*=inverse;return pose;},preserveAddedRootWorld:true);
            return Stationary(appended,duration);
        }
        foreach(var (id,file) in old)generated.Add(id,Motion(id,file.Bytes.ToArray(),out _));
        var project=bank.Files.Single(f=>f.ID==7000300);var projectBytes=project.Bytes.ToArray();
        foreach(var row in tae.Animations) {
            int id=checked((int)row.ID),sourceId=SourceSlot(source,id),physical=Physical(id);bool copied=!generated.ContainsKey(id);
            if(physical<0)physical=id/1000000*1000000;
            Need(old.ContainsKey(physical),"unavailable native fallback carrier: "+id);
            bool additive;
            if(copied){generated.Add(id,Motion(id,old[physical].Bytes.ToArray(),out additive));aliases++;}
            else {var p=new NativeAnimation.Pack(old[id].Bytes.ToArray());additive=old[id].Bytes.Span[p.Data+p.Object("hkaAnimationBinding")+80]==1;}
            var original=source.Resolve(sourceId);float duration=original.Clip.Duration;int hits=0,shots=0;
            row.MiniHeader=new TAE.Animation.AnimMiniHeader.Standard{IsLoopByDefault=(originalRows[id] as TAE.Animation.AnimMiniHeader.Standard)?.IsLoopByDefault??false,ImportsHKX=false,ImportHKXSourceAnimID=-1};
            row.AnimFileName=$"a{id/1000000:D3}_{id%1000000:D6}.hkt";
            // Every contact/projectile/effect in this private bank is owned;
            // donor attack, sound, blood and special-effect events are removed.
            row.Events.Clear();row.EventGroups.Clear();var groups=new Dictionary<(int Type,int Key),TAE.EventGroup>();
            foreach(var e in original.Events.Where(e=>e.Type==0||!additive&&id%1000000 is >=3000 and <=3008&&e.Type is 1 or 2)) {
                var args=e.GetParameterBytes(false);Need(float.IsFinite(e.StartTime)&&float.IsFinite(e.EndTime)&&e.StartTime>=0&&e.EndTime>=e.StartTime&&e.EndTime<=600,"source event time budget");
                int type=e.Type,key=type==0?BitConverter.ToInt32(args):0;TAE.Event prototype=type==0?gate:type==1?hit:shot;byte[] payload;
                if(type==0){Need(args.Length==12,"source state event ABI");payload=new byte[16];args.CopyTo(payload,0);}
                else if(type==1){Need(args.Length==12&&BitConverter.ToInt32(args)==0&&BitConverter.ToInt32(args,4) is >=0 and <=8,"source contact ABI");payload=new byte[16];args.CopyTo(payload,0);hits++;}
                else {Need(args.Length==16,"source projectile ABI");int judge=BitConverter.ToInt32(args,8);if(judge==593){suppressed++;continue;}payload=new byte[16];args.CopyTo(payload,0);BitConverter.GetBytes(HydraCharacters.Socket(BitConverter.ToInt32(args))).CopyTo(payload,0);shots++;}
                if(!groups.TryGetValue((type,key),out var group)){group=prototype.Group!.GetClone();row.EventGroups.Add(group);groups.Add((type,key),group);}
                float start=Math.Min(e.StartTime,duration),end=Math.Min(e.EndTime,duration);if(end<start)end=start;
                row.Events.Add(new TAE.Event(start,end,type,prototype.Unk04,payload,false){Group=group});
            }
            // Native HKS needs its death-state gate even where source Hydra's
            // large-body death action used a different event family.
            if(id%1000000 is >=2200 and <2300&&!row.Events.Any(e=>e.Type==0&&BitConverter.ToInt32(e.GetParameterBytes(false))==12)) {
                var group=gate.Group!.GetClone();row.EventGroups.Add(group);var bytes=new byte[16];BitConverter.GetBytes(12).CopyTo(bytes,0);BitConverter.GetBytes(-1).CopyTo(bytes,8);
                row.Events.Add(new TAE.Event(Math.Max(0,duration-.1f),duration,0,gate.Unk04,bytes,false){Group=group});
            }
            if(copied) {
                string from=@"Model\chr\c9999\hkx\"+$"a{id/1000000:D3}_{id%1000000:D6}.hkx";var pattern=Encoding.ASCII.GetBytes(from+'\0');int at=projectBytes.AsSpan().IndexOf(pattern);
                if(at>=0){Need(projectBytes.AsSpan(at+1).IndexOf(pattern)<0,"duplicate native common project path");Encoding.ASCII.GetBytes(HydraCharacters.Model).CopyTo(projectBytes,at+from.IndexOf("c9999",StringComparison.Ordinal));}
            }
            bindings.Add(new(id,sourceId,original.MotionID,original.Archive,duration,hits,shots,copied,additive));
        }
        foreach(var file in bank.Files.Where(f=>f.ID>=1000000000))file.Bytes=generated[file.ID-1000000000];
        foreach(var (id,bytes) in generated.Where(p=>!old.ContainsKey(p.Key)))bank.Files.Add(new BinderFile(Binder.FileFlags.Flag1,1000000000+id,@"N:\SPRJ\data\INTERROOT_ps4\chr\c8830\hkx\"+$"a{id/1000000:D3}_{id%1000000:D6}.hkx",bytes));
        project.Bytes=projectBytes;entry.Bytes=tae.Write();
        return new(159,aliases,tae.Animations.Count,generated.Count,suppressed,bindings,[
            "All native physical slots and owned event aliases carry the complete source visual skeleton; non-additive clips use source duration and stationary extracted motion. Native controller channels and graph routing remain Bloodborne's.",
            "Source requested3008 keeps its own projectile events while resolving physical3004; source recoil8000 uses native7005 in both banks.",
            "Unresolved source judge593 splash pulses are suppressed. Source sound, noncombat FX, camera and unsupported event families are not converted; attack/projectile damage is supplied exclusively through owned source1/2 events."]);
    }
}
