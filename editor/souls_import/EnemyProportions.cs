// SPDX-License-Identifier: GPL-3.0-or-later
// The spline layout follows SourceAnimation and Smithbox's GPL spline reader.
// Change positional channels in place: preserve native rotations, compression,
// clip timing, extracted motion and animation/behavior event identities.
#nullable enable
using SoulsFormats;
using System.Numerics;

internal static class EnemyProportions {
    internal sealed record Segment(string Bone,string? Ancestor,float SourceLength,float NativeLength,float Factor);
    internal sealed record Plan(byte[] Model,byte[] Skeleton,NativeAnimation.Rig Original,float[] Factors,float[] ModelFactors,IReadOnlyList<Segment> Segments);
    static string Key(string value)=>new(value.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
    internal static Plan Create(byte[] sourceBytes,byte[] nativeBytes,byte[] skeleton) {
        var source=FLVER2.Read(sourceBytes);var model=FLVER2.Read(nativeBytes);var rig=NativeAnimation.Skeleton(skeleton);
        if(rig.Names.Length!=80||rig.Name!="Master"||rig.Names[0]!="Master"||rig.Parents[0]!=-1)
            throw new InvalidDataException("Ordinary hollow proportions require the native c2610 rig");
        var sw=SkinRetarget.World(source.Nodes);var mw=SkinRetarget.World(model.Nodes);
        var nativeNodes=rig.Names.Select(name=>model.Nodes.FindIndex(n=>n.Name==name)).ToArray();
        if(nativeNodes.Any(i=>i<0)||nativeNodes.Distinct().Count()!=rig.Names.Length)throw new InvalidDataException("Missing hollow bind-pose bones");
        for(int i=0;i<rig.Names.Length;i++)if(rig.Names[i]!="Pelvis"&&model.Nodes[nativeNodes[i]].ParentIndex!=(rig.Parents[i]<0?-1:nativeNodes[rig.Parents[i]]))
            throw new InvalidDataException("Hollow bind/animation hierarchies disagree at "+rig.Names[i]+": "+model.Nodes[nativeNodes[i]].ParentIndex+"/"+rig.Parents[i]);
        var sources=source.Nodes.Select((n,i)=>(key:Key(n.Name),i)).GroupBy(p=>p.key).Where(g=>g.Count()==1).ToDictionary(g=>g.Key,g=>g.Single().i);
        var mapped=rig.Names.Select(n=>sources.GetValueOrDefault(Key(n),-1)).ToArray();
        // Source twist helpers can sit halfway down a limb while c2610's are
        // coincident with the joint. Their skin mapping remains available, but
        // they do not define a new anatomical segment or change the limb fit.
        for(int i=0;i<mapped.Length;i++)if(rig.Names[i].Contains("Twist",StringComparison.OrdinalIgnoreCase))mapped[i]=-1;
        foreach(string bone in new[]{"Pelvis","Spine","Spine1","Neck","Head","L UpperArm","L Forearm","L Hand","R UpperArm","R Forearm","R Hand","L Thigh","L Calf","L Foot","R Thigh","R Calf","R Foot"}) {
            int index=Array.IndexOf(rig.Names,bone);
            if(index<0||mapped[index]<0)throw new InvalidDataException("Missing ordinary hollow joint: "+bone);
        }
        float stature=SkinRetarget.HumanoidScale(source.Nodes,model.Nodes);
        // DS1's root is Pelvis. c2610 puts its height on Master and leaves
        // Pelvis at zero; align that root height rather than stretching Pelvis.
        int pelvis=Array.IndexOf(rig.Names,"Pelvis");
        if(rig.Parents[pelvis]!=0||model.Nodes[nativeNodes[pelvis]].ParentIndex!=-1||rig.Local[pelvis].Translation.Length()>.0001f||source.Nodes[mapped[pelvis]].ParentIndex!=-1)
            throw new InvalidDataException("Unsupported hollow pelvis/root layout");
        mapped[0]=mapped[pelvis];
        var factors=Enumerable.Repeat(stature,rig.Names.Length).ToArray();
        factors[0]=sw[mapped[0]].Translation.Length()/rig.Local[0].Translation.Length();
        var modelFactors=Enumerable.Repeat(stature,rig.Names.Length).ToArray();
        modelFactors[0]=sw[mapped[0]].Translation.Length()/mw[nativeNodes[0]].Translation.Length();
        modelFactors[pelvis]=sw[mapped[0]].Translation.Length()/mw[nativeNodes[pelvis]].Translation.Length();
        var assigned=new bool[rig.Names.Length];assigned[0]=assigned[pelvis]=true;
        var segments=new List<Segment>();
        // Fit the shared torso helper before its clavicle branches. Once a
        // helper has a length, later branches solve only their remaining edge.
        var order=new[]{"Pelvis","Spine","Spine1","Neck","Head"}.Select(n=>Array.IndexOf(rig.Names,n)).Concat(Enumerable.Range(1,rig.Names.Length-1)).Distinct();
        foreach(int bone in order) {
            if(mapped[bone]<0)continue;
            var chain=new List<int>();int at=bone;
            while(at>0){chain.Add(at);at=rig.Parents[at];if(at<0||mapped[at]>=0)break;}
            Vector3 origin=at>=0&&mapped[at]>=0?sw[mapped[at]].Translation:Vector3.Zero;
            float sourceLength=Vector3.Distance(sw[mapped[bone]].Translation,origin),nativeLength=0,modelLength=0,fixedNative=0,fixedModel=0,freeNative=0,freeModel=0;
            foreach(int child in chain) {
                float nl=Vector3.Distance(rig.World[child].Translation,rig.Parents[child]<0?Vector3.Zero:rig.World[rig.Parents[child]].Translation);
                float ml=Vector3.Distance(mw[nativeNodes[child]].Translation,rig.Parents[child]<0?Vector3.Zero:mw[nativeNodes[rig.Parents[child]]].Translation);
                nativeLength+=nl;modelLength+=ml;
                if(assigned[child]){fixedNative+=nl*factors[child];fixedModel+=ml*modelFactors[child];}else{freeNative+=nl;freeModel+=ml;}
            }
            if(nativeLength<.00001f) {if(sourceLength>.001f)throw new InvalidDataException("Incompatible zero-length hollow joint: "+rig.Names[bone]);continue;}
            float factor=(sourceLength-fixedNative)/freeNative;
            float modelFactor=(sourceLength-fixedModel)/freeModel;
            if(!float.IsFinite(factor)||factor is <.05f or >4||!float.IsFinite(modelFactor)||modelFactor is <.05f or >4)throw new InvalidDataException("Unsupported hollow segment ratio: "+rig.Names[bone]);
            foreach(int child in chain)if(!assigned[child]){factors[child]=factor;modelFactors[child]=modelFactor;assigned[child]=true;}
            segments.Add(new(rig.Names[bone],at>=0?rig.Names[at]:null,sourceLength,nativeLength,sourceLength/nativeLength));
        }
        var byName=rig.Names.Select((name,i)=>(name,i)).ToDictionary(p=>p.name,p=>p.i,StringComparer.Ordinal);
        // c2610's model bind and Havok reference poses are not identical. Fit
        // each representation's own segment lengths; retain their rotations.
        foreach(var node in model.Nodes)if(byName.TryGetValue(node.Name,out int bone))node.Translation*=modelFactors[bone];
        var reference=skeleton.ToArray();var pack=new NativeAnimation.Pack(reference);var poses=pack.Array(pack.Object("hkaSkeleton")+56,48,256);
        for(int bone=0;bone<rig.Names.Length;bone++)for(int axis=0;axis<3;axis++) {
            int offset=pack.Data+poses.At+bone*48+axis*4;
            BitConverter.GetBytes(BitConverter.ToSingle(reference,offset)*factors[bone]).CopyTo(reference,offset);
        }
        return new(model.Write(),reference,rig,factors,modelFactors,segments);
    }

    internal static byte[] Clip(byte[] input,Plan plan) {
        var output=input.ToArray();var p=new NativeAnimation.Pack(input);int animation=p.Object("hkaSplineCompressedAnimation"),binding=p.Object("hkaAnimationBinding");
        int frames=p.Int(animation+56),blocks=p.Int(animation+60),blockFrames=p.Int(animation+64),tracks=p.Int(animation+20);
        if(p.Int(animation+12)!=3||tracks!=plan.Factors.Length||p.Int(animation+24)!=0||p.Int(animation+168)!=0||
            frames is <2 or >65536||blocks is <1 or >256||blockFrames!=256||blocks!=(frames-2)/255+1||input[p.Data+binding+80]>1||p.String(binding+16)!=plan.Original.Name)
            throw new InvalidDataException("Unsupported native hollow spline header");
        var map=p.Array(binding+32,2,256);
        if(map.Count!=tracks||Enumerable.Range(0,tracks).Any(i=>p.Short(map.At+i*2)!=i))throw new InvalidDataException("Native hollow spline bone bindings changed");
        var data=p.Array(animation+152,1,64*1024*1024);var starts=p.Array(animation+88,4,256);var floatEnds=p.Array(animation+104,4,256);
        if(starts.Count!=blocks||floatEnds.Count!=blocks||p.Int(starts.At)!=0)throw new InvalidDataException("Invalid native hollow spline blocks");
        for(int block=0;block<blocks;block++) {
            int start=p.Int(starts.At+block*4),end=block+1<blocks?p.Int(starts.At+(block+1)*4):data.Count;
            int floatEnd=p.Int(floatEnds.At+block*4),last=Math.Min(255,frames-1-block*255);
            if(start<0||start%16!=0||end<=start||end>data.Count||floatEnd<0||floatEnd>end-start)throw new InvalidDataException("Invalid hollow spline block range");
            int cursor=0,absolute=p.Data+data.At+start,limit=end-start;
            void Skip(int count){if(count<0||cursor>limit-count)throw new InvalidDataException("Truncated hollow spline channel");cursor+=count;}
            byte Byte(){Skip(1);return input[absolute+cursor-1];}
            void Align(int alignment)=>Skip((alignment-cursor%alignment)%alignment);
            int Curve() {
                int n=Byte()|(Byte()<<8);n++;int degree=Byte();
                if(n is <2 or >260||degree is <1 or >4||n<degree+1)throw new InvalidDataException("Unsupported hollow spline curve");
                var knots=new byte[n+degree+1];for(int i=0;i<knots.Length;i++)knots[i]=Byte();
                if(knots[degree]!=0||knots[n]!=last||knots.Zip(knots.Skip(1)).Any(v=>v.First>v.Second))throw new InvalidDataException("Invalid hollow spline knot domain");return n;
            }
            void Scalar(float factor) {
                Skip(4);int offset=absolute+cursor-4;float before=BitConverter.ToSingle(input,offset),after=before*factor;
                if(!float.IsFinite(before)||!float.IsFinite(after)||Math.Abs(after)>100000)throw new InvalidDataException("Invalid hollow spline position");
                if(factor!=1)BitConverter.GetBytes(after).CopyTo(output,offset);
            }
            void Vector(byte mask,float factor) {
                int points=(mask&0x70)!=0?Curve():0;if(points>0)Align(4);
                for(int axis=0;axis<3;axis++)if((mask&(16<<axis))!=0){Scalar(factor);Scalar(factor);}else if((mask&(1<<axis))!=0)Scalar(factor);
                int dynamicAxes=BitOperations.PopCount((uint)(mask&0x70));Skip(checked(points*dynamicAxes*2));
            }
            var masks=new (byte Position,byte Rotation,byte Scale)[tracks];
            for(int track=0;track<tracks;track++) {
                if(Byte()!=0x45)throw new InvalidDataException("Native hollow spline quantization changed");
                byte position=Byte(),rotation=Byte(),scale=Byte();
                if((position&0x88)!=0||(scale&0x88)!=0||((position&7)&(position>>4))!=0||((scale&7)&(scale>>4))!=0)
                    throw new InvalidDataException("Invalid hollow spline vector masks");masks[track]=(position,rotation,scale);
            }
            Align(4);
            for(int track=0;track<tracks;track++) {
                var mask=masks[track];Vector(mask.Position,plan.Factors[track]);Align(4);
                if((mask.Rotation&0xf0)!=0)Skip(checked(Curve()*5));else if(mask.Rotation!=0)Skip(5);
                Align(4);Vector(mask.Scale,1);Align(4);
            }
            if(cursor!=floatEnd)throw new InvalidDataException("Native hollow transform channels do not match their declared endpoint");
            Align(16);if(cursor!=limit)throw new InvalidDataException("Unexpected native hollow spline trailing data");
        }
        return output;
    }
}
