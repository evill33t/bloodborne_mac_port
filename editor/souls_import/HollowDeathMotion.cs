// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Text;

// Four common-bank death actions otherwise animate only Bloodborne's original
// skeleton. Own these clips explicitly; leave every other shared action alone.
internal static class HollowDeathMotion {
    internal sealed record Binding(int Action,int NativeCounterpart,int SourceAction,float NativeDuration,float SourceDuration,int ProjectOffset);
    internal static IReadOnlyList<Binding> Apply(BND4 bank,NativeRigAppend.Plan rig,HollowCombatMotion.Plan motion) {
        var project=bank.Files.Single(f=>f.ID==7000300);var bytes=project.Bytes.ToArray();
        var tae=TAE.Read(bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);
        var pending=new List<BinderFile>();var bindings=new List<Binding>();
        foreach(var pair in new (int Action,int Counterpart,int Source)[]{(2002211,2002200,2200),(2002261,2002250,2250),(3002252,3002250,2250),(3002253,3002250,2250)}) {
            var action=tae.Animations.Single(a=>a.ID==pair.Action);
            if(action.MiniHeader is not TAE.Animation.AnimMiniHeader.Standard header||header.ImportsHKX||header.ImportHKXSourceAnimID!=-1||bank.Files.Any(f=>f.ID==1000000000+pair.Action))
                throw new InvalidDataException("Native shared death action contract changed");
            string name=$"a{pair.Action/1000000:D3}_{pair.Action%1000000:D6}",from=$"a{pair.Counterpart/1000000:D3}_{pair.Counterpart%1000000:D6}";
            string path=@"Model\chr\c9999\hkx\"+name+".hkx";var pattern=Encoding.ASCII.GetBytes(path+"\0");
            int offset=bytes.AsSpan().IndexOf(pattern);
            if(offset<0||bytes.AsSpan(offset+1).IndexOf(pattern)>=0)throw new InvalidDataException("Native shared death project path changed");
            var original=bank.Files.Single(f=>f.ID==1000000000+pair.Counterpart);
            string leaf=original.Name.Replace('\\','/').Split('/').Last();
            if(!leaf.Equals(from+".hkx",StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Native death counterpart filename changed");
            // Counterparts already contain the selected source death tracks.
            // Clone their native physical channels and timing explicitly.
            if(!motion.MotionSourceIDs.TryGetValue(pair.Counterpart,out int sourceAction)||sourceAction%10000!=pair.Source||motion.ReferenceFallbackIDs.Contains(pair.Counterpart))
                throw new InvalidDataException("Native death counterpart has no authored role death variant");
            var clip=motion.Clip(pair.Counterpart);var p=new NativeAnimation.Pack(original.Bytes.ToArray());int anim=p.Object("hkaSplineCompressedAnimation");
            if(p.Int(anim+20)!=rig.Count)throw new InvalidDataException("Death counterpart has no owned source tracks");
            // Equipped styles can select12200/22200 etc. Retain that exact
            // role variant; a base2200-only guard rejects valid source banks.
            pending.Add(new BinderFile(original.Flags,1000000000+pair.Action,original.Name.Replace(from,name,StringComparison.OrdinalIgnoreCase),original.Bytes.ToArray()));
            Encoding.ASCII.GetBytes("c2630").CopyTo(bytes,offset+path.IndexOf("c9999",StringComparison.Ordinal));
            bindings.Add(new(pair.Action,pair.Counterpart,sourceAction,p.Float(anim+16),clip.Duration,offset));
        }
        project.Bytes=bytes;bank.Files.AddRange(pending);bank.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));return bindings;
    }
}
