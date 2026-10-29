// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class MapEnvironmentTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Near(float a,float b)=>Check(Math.Abs(a-b)<.0001f,$"Unexpected environment value: {a} != {b}");
    static void Reject(Action action){try{action();}catch(InvalidDataException){return;}throw new Exception("Invalid environment accepted");}
    static GPARAM.IField F<T>(GPARAM.Field<T> field,string key,T value){field.Key=key;field.Capacity=1;field.Values=[new(){ID=0,Value=value}];return field;}
    static GPARAM Profile() {
        var g=new GPARAM{Version=GPARAM.GparamVersion.V3,Data30=[7,9,13,17],UnkParamExtras=[],Unk40=1.25f};var light=new GPARAM.Param{Key="LightSet ParamEditor"};
        foreach(var suffix in new[]{"0","1","LocalLight Angle1","LocalLight Angle2"})light.Fields.Add(F(new GPARAM.Vector2Field(),suffix.Length==1?"Directional Light Angle"+suffix:"Directional Light "+suffix,Vector2.One));
        foreach(var suffix in new[]{"0","1","LocalLight DiffColor1","LocalLight DiffColor2"})light.Fields.Add(F(new GPARAM.Vector4Field(),suffix.Length==1?"Directional Light DiffColor"+suffix:"Directional Light "+suffix,Vector4.One));
        foreach(var suffix in new[]{"0","1","LocalLight SpecColor1","LocalLight SpecColor2"})light.Fields.Add(F(new GPARAM.Vector4Field(),suffix.Length==1?"Directional Light SpecColor"+suffix:"Directional Light "+suffix,Vector4.One));
        foreach(var side in new[]{"Up","Down"})light.Fields.Add(F(new GPARAM.Vector4Field(),"Directional Light Hemi Color "+side,Vector4.One));
        foreach(var key in new[]{"Directional LightLight1 FollowedCamera","Directional Enable LocalLight1","Directional Enable LocalLight2"})light.Fields.Add(F(new GPARAM.BoolField(),key,false));g.Params.Add(light);
        var scatter=new GPARAM.Param{Key="LightScattering : ParamEditor"};
        var keys=new[]{"Ls-sunRotX","Ls-sunRotY","Ls-距離倍率[%]","Ls-光源色R","Ls-光源色G","Ls-光源色B","Ls-光源RGB倍率[%]","Ls-散乱方向係数","Ls-レイリー散乱係数","Ls-ミー散乱係数","Ls-ブレンド係数[%]","Ls-光源色R","Ls-光源色G","Ls-光源色B","Ls-地上の乱反射光RGB倍率[%]"};
        for(int i=0;i<keys.Length;i++)scatter.Fields.Add(i is 7 or 8 or 9?F(new GPARAM.FloatField(),keys[i],.5f):F(new GPARAM.ShortField(),keys[i],(short)9));g.Params.Add(scatter);
        g.Params.Add(new(){Key="FogParamEditor",Fields=[F(new GPARAM.FloatField(),"Fog-fogAlpha",.7f),F(new GPARAM.FloatField(),"Fog-heightFogDensity",14f),F(new GPARAM.Vector4Field(),"Fog-Color",Vector4.One),F(new GPARAM.Vector4Field(),"Fog-heightColor",Vector4.One),F(new GPARAM.FloatField(),"Fog-Dist",42f),F(new GPARAM.FloatField(),"Fog-DistOffset",20f),F(new GPARAM.FloatField(),"Fog-fogContrast",.46f)]});
        g.Params.Add(new(){Key="ShadowParam ParamEditor",Fields=[F(new GPARAM.Vector2Field(),"hadowParam - Shadow: Angle0",new Vector2(-.45f,2.05f)),F(new GPARAM.FloatField(),"ShadowParam - Shadow: Near",.1f),F(new GPARAM.FloatField(),"ShadowParam - DepthOffsetScale",-.085f),F(new GPARAM.ColorField(),"Directional Shadow Color",System.Drawing.Color.FromArgb(200,23,14,7)),F(new GPARAM.FloatField(),"ShadowParam - Depth Smooth Compare",.005f),F(new GPARAM.FloatField(),"ShadowParam - Depth Smooth Compare",.006f)]});
        g.Params.Add(new(){Key="Tone Map[Yebis]",Fields=[F(new GPARAM.FloatField(),"Yebis-ToneMapExposure",3f),F(new GPARAM.IntField(),"Yebis-AutoExposure",0),F(new GPARAM.FloatField(),"Yebis-Gamma",2.2f),F(new GPARAM.FloatField(),"Yebis-FilmicContrast",.24f),F(new GPARAM.FloatField(),"GS Presents - ToneMap-Exposure",0f)]});
        g.Params.Single(p=>p.Key=="Tone Map[Yebis]").Fields.AddRange([F(new GPARAM.FloatField(),"Yebis-ColorS",.9f),F(new GPARAM.FloatField(),"Yebis-ColorV",1f),F(new GPARAM.FloatField(),"Yebis-ColorH",0f),F(new GPARAM.FloatField(),"Yebis-Contrust",1f),F(new GPARAM.IntField(),"Yebis-Chromatic Aberration Enable",1)]);
        g.Params.Add(new(){Key="ColorGrading[Yebis]",Fields=[F(new GPARAM.IntField(),"Yebis-ColorGrading-Enable",1),F(new GPARAM.IntField(),"Yebis-LutSourceId",4)]});
        g.Params.Add(new(){Key="NativeUnrelated",Fields=[new GPARAM.FloatField{Key="sentinel",Capacity=1,Values=[new(){ID=30000,Value=123.25f}]}]});
        // Native group indexes can reserve IDs absent from any field. Keep their
        // historical order; adding serialized values must also register the ID.
        g.UnkParamExtras=[new(){GroupIndex=0,Ids=[24120,30001,0]}];return g;
    }
    static PARAM Parameter(string type,params(int ID,Dictionary<string,float> Cells)[] rows) {
        var def=new PARAMDEF{ParamType=type};foreach(var key in rows.SelectMany(r=>r.Cells.Keys).Distinct())def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.f32,key){Default=0f});
        var p=new PARAM{ParamType=type,Rows=[]};foreach(var input in rows){var row=new PARAM.Row(input.ID,"fixture",def);foreach(var cell in input.Cells)row.Cells.Single(c=>c.Def.InternalName==cell.Key).Value=cell.Value;p.Rows.Add(row);}return p;
    }
    static (PARAM Light,PARAM Fog,PARAM Scatter) Source() {
        var light=new Dictionary<string,float>();
        foreach(var suffix in new[]{"0","1","2","s","u","d"}){foreach(var color in new[]{"R","G","B"})light["col"+color+"_"+suffix]=255;light["colA_"+suffix]=50;}
        foreach(var item in new[]{("0",40f,75f),("1",30f,70f),("2",-60f,-130f),("s",30f,30f)}){light["degRotX_"+item.Item1]=item.Item2;light["degRotY_"+item.Item1]=item.Item3;}
        light["colA_0"]=150;light["colR_u"]=245;light["colA_u"]=40;light["colA_d"]=25;var foliage=new Dictionary<string,float>(light){["colA_u"]=60};
        var fog=new Dictionary<string,float>{{"fogBeginZ",-150},{"fogEndZ",70},{"degRotW",65},{"colR",95},{"colG",95},{"colB",92},{"colA",120}};
        var scatter=new Dictionary<string,float>{{"sunRotX",70},{"sunRotY",150},{"distanceMul",130},{"sunR",70},{"sunG",200},{"sunB",220},{"sunA",300},{"lsHGg",.35f},{"lsBetaRay",.1f},{"lsBetaMie",.6f},{"blendCoef",50},{"reflectanceR",205},{"reflectanceG",225},{"reflectanceB",255},{"reflectanceA",100}};
        return(Parameter("LIGHT_BANK",(20,light),(23,foliage)),Parameter("FOG_BANK",(20,fog)),Parameter("LIGHT_SCATTERING_BANK",(20,scatter)));
    }
    static T Value<T>(GPARAM g,string group,string field,int id)=>((GPARAM.Field<T>)g.Params.Single(p=>p.Key==group).Fields.Single(f=>f.Key==field)).Values.Single(v=>v.ID==id).Value;
    static FLVER2 Sky(bool native) {
        var f=new FLVER2();f.Header.Version=native?0x20014:0x2000C;f.Nodes=[new(){Name="root",ParentIndex=-1,Scale=Vector3.One}];
        f.BufferLayouts=[new(){new(FLVER.LayoutType.Float3,FLVER.LayoutSemantic.Position),new(FLVER.LayoutType.Float3,FLVER.LayoutSemantic.Normal),new(FLVER.LayoutType.Float4,FLVER.LayoutSemantic.VertexColor,1),new(FLVER.LayoutType.Float2,FLVER.LayoutSemantic.UV)}];
        foreach(var name in native?new[]{"M_Sky_daytime_[Dn].mtd","M_Sky_daytime_[Dn]_Alp.mtd"}:new[]{"A10_bad_landscape.mtd","A10_02_m9000_cloud[DB]_Alp.mtd","A10_02_m9000_Sky[Dn]_LS.mtd"}) {
            f.Materials.Add(new("sky",name,0){Textures=[new(){ParamName=native?"g_DiffuseTexture":"g_Diffuse",Path="sky.tif"}]});var mesh=new FLVER2.Mesh{UseBoneWeights=false,MaterialIndex=f.Materials.Count-1,NodeIndex=0,BoneIndices=[0],VertexBuffers=[new(0)]};
            foreach(var position in new[]{new Vector3(0,2,0),new Vector3(1,2,0),new Vector3(0,2,1)}){var v=new FLVER.Vertex{Position=position,Normal=Vector3.UnitY,Colors=[new(.25f,.5f,.75f,1f)],UVs=[new(.2f,.4f,0)]};v.BoneIndices[0]=0;mesh.Vertices.Add(v);}
            if(name.Contains("bad_landscape"))mesh.Vertices[0].Position=new(float.NaN,0,0);mesh.FaceSets=[new(FLVER2.FaceSet.FSFlags.None,false,false,0,[0,1,2])];f.Meshes.Add(mesh);
        }return f;
    }
    static MTD Definition(bool source,float power)=>new(){Params=[new MTD.Param(source?"g_DiffuseMapColor":"g_DiffuseColor",MTD.ParamType.Float3,new float[]{1,.8f,.6f}),new MTD.Param(source?"g_DiffuseMapColorPower":"g_DiffuseColorPower",MTD.ParamType.Float,power)]};
    static void LightEnergy() {
        // Independent transfer equations recovered from shipped DSR 0x140564510
        // (powf exponent at 0x1412aa960) and BB 0x022de8b0 / 0x022e0b03.
        // Validate the resulting native light energy after GPARAM serialization.
        var source=Source();var profiles=new Dictionary<string,byte[]>{{"phase",Profile().Write()}};
        var fields=new[]{"Directional Light DiffColor0","Directional Light DiffColor1","Directional Light LocalLight DiffColor1","Directional Light LocalLight SpecColor2","Directional Light Hemi Color Up","Directional Light Hemi Color Down"};
        foreach(var sample in new[]{(R:0f,G:0f,B:0f,A:0f),(R:245f,G:127f,B:4f,A:40f),(R:12f,G:83f,B:240f,A:500f),(R:255f,G:50f,B:1f,A:150f),(R:100f,G:200f,B:255f,A:-50f)}) {
            foreach(var suffix in new[]{"0","1","2","s","u","d"})foreach(var channel in new[]{("R",sample.R),("G",sample.G),("B",sample.B),("A",sample.A)})
                source.Light.Rows[0].Cells.Single(c=>c.Def.InternalName=="col"+channel.Item1+"_"+suffix).Value=channel.Item2;
            var result=MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,[new("ground",20,20,20)],"DS1R");
            var g=GPARAM.Read(result.Files.Single().Bytes);int id=result.Bindings["ground"].LightSetID;
            foreach(var field in fields) {
                var c=Value<Vector4>(g,"LightSet ParamEditor",field,id);var rgb=new[]{c.X,c.Y,c.Z};var original=new[]{sample.R,sample.G,sample.B};
                for(int n=0;n<3;n++) {
                    double expected=Math.Pow(Math.Abs(original[n]/255d*sample.A/100d),2.2),actual=(double)rgb[n]*rgb[n]*c.W;
                    Check(Math.Abs(expected-actual)<=.00001*Math.Max(1,expected),$"DS1R source light energy changed in {field}: {actual} != {expected}");
                }
            }
            Check(result.Rows.Single(r=>r.Kind=="Light").Status.Contains("DS1R light color energy"),"Light transfer provenance omitted");
        }
        var legacy=MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,[new("ground",20,20,20)],"DS1");
        var legacyColor=Value<Vector4>(GPARAM.Read(legacy.Files.Single().Bytes),"LightSet ParamEditor","Directional Light DiffColor0",legacy.Bindings["ground"].LightSetID);
        Near(legacyColor.X,100f/255);Near(legacyColor.W,-.5f);
        Check(legacy.Rows.Single(r=>r.Kind=="Light").Status.Contains("has not been binary-verified"),"Original DS1 response was claimed verified");
        Reject(()=>MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,[],"DS2S"));
    }
    static void ShadowDirection() {
        var source=Source();var input=Profile().Write();var profiles=new Dictionary<string,byte[]>{{"phase0",input},{"phase1",input}};
        var shadow=Parameter("SHADOW_BANK",(20,new(){{"lightDegRotX",50},{"lightDegRotY",160},{"densityRatio",100},{"persedDepthOffset",-.03f},{"shadowVolumeDepth",1}}),(21,new(){{"lightDegRotX",80},{"lightDegRotY",60},{"densityRatio",90},{"persedDepthOffset",-.07f},{"shadowVolumeDepth",3}}));
        EnvironmentBinding[] bindings=[new("ground",20,20,20),new("floor",20,20,20,20,true),new("floor2",20,20,20,20,true),new("cave",20,20,20,21,true),new("disabled",20,20,20,255,true)];
        var result=MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,bindings,"DS1R",shadow,new(false,0));
        Check(result.ShadowBindings.Count==3&&!result.ShadowBindings.ContainsKey("ground")&&!result.ShadowBindings.ContainsKey("disabled"),"Scene shadow bindings include unrelated parts or disabled source IDs");
        Check(result.ShadowBindings["floor"]==result.ShadowBindings["floor2"]&&result.ShadowBindings["floor"]!=result.ShadowBindings["cave"],"Source shadow rows were conflated or unnecessarily duplicated");
        foreach(var file in result.Files) {
            var original=GPARAM.Read(input);var g=GPARAM.Read(file.Bytes);var before=original.Params.Single(p=>p.Key=="ShadowParam ParamEditor");var after=g.Params.Single(p=>p.Key==before.Key);
            int id=result.ShadowBindings["floor"];var angle=Value<Vector2>(g,before.Key,"hadowParam - Shadow: Angle0",id);Near(angle.X,50*MathF.PI/180);Near(angle.Y,160*MathF.PI/180);
            int group=g.Params.IndexOf(after);Check(g.UnkParamExtras.Single(e=>e.GroupIndex==group).Ids.Contains(id),"Added shadow row is not registered for native runtime lookup");
            var direction=new Vector3(MathF.Sin(angle.Y)*MathF.Cos(angle.X),-MathF.Sin(angle.X),MathF.Cos(angle.Y)*MathF.Cos(angle.X));
            Near(direction.X,.2198463f);Near(direction.Y,-.76604444f);Near(direction.Z,-.6040228f);
            Near(Value<Vector2>(g,before.Key,"hadowParam - Shadow: Angle0",result.ShadowBindings["cave"]).X,80*MathF.PI/180);
            for(int f=0;f<before.Fields.Count;f++) {
                var old=before.Fields[f];var current=after.Fields[f];Check(current.Values.Count==3,"New shadow rows or field capacities were not serialized");
                Check(Equals(old.Values.Single().Value,current.Values.Single(v=>v.ID==0).Value),"Existing native shadow value changed");
                if(old.Key!="hadowParam - Shadow: Angle0")foreach(int target in result.ShadowBindings.Values.Distinct())Check(Equals(old.Values.Single().Value,current.Values.Single(v=>v.ID==target).Value),"Unverified native shadow guard changed");
            }
        }
        Check(result.Rows.Count(r=>r.Kind=="Shadow")==3&&result.Rows.Single(r=>r.Kind=="Shadow"&&r.SourceID==255).NativeID==null,"Disabled-shadow provenance lost");
        Check((float)result.Rows.Single(r=>r.Kind=="Shadow"&&r.SourceID==20).Values["persedDepthOffset"]==-.03f,"Unconverted source bias provenance lost");
        Reject(()=>MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,[bindings[1]],"DS1R"));
        Reject(()=>MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,[bindings[1] with{ShadowID=19}],"DS1R",shadow));
        shadow.Rows[0].Cells.Single(c=>c.Def.InternalName=="lightDegRotX").Value=float.NaN;
        Reject(()=>MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,[bindings[1]],"DS1R",shadow));
    }
    static void FogToneCalibration() {
        var source=Source();var phase0=Profile();var phase1=Profile();
        ((GPARAM.FloatField)phase1.Params.Single(p=>p.Key=="Tone Map[Yebis]").Fields.Single(f=>f.Key=="Yebis-ToneMapExposure")).Values[0].Value=5;
        var profiles=new Dictionary<string,byte[]>{{"phase0",phase0.Write()},{"phase1",phase1.Write()}};
        EnvironmentBinding[] bindings=[new("ground",20,20,20),new("floor",20,20,20,255,true,20),new("floor2",20,20,20,255,true,20),new("cave",20,255,20,255,true,21)];
        var tone=Parameter("TONE_MAP_BANK",(20,new(){{"grayKeyValue",.18f},{"inverseToneMapMul",3}}),(21,new(){{"grayKeyValue",.2f},{"inverseToneMapMul",4}}));
        var calibration=new EnvironmentCalibration();var result=MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,bindings,"DS1R",calibration:calibration,toneMap:tone);
        Check(result.Calibration==calibration&&result.SceneBindings.Count==3&&!result.SceneBindings.ContainsKey("ground"),"Calibration or collision-only tone bindings lost");
        int fog=result.Bindings["ground"].FogParamID,noFog=result.Bindings["cave"].FogParamID,toneID=result.SceneBindings["floor"].ToneMapID;
        Check(fog!=0&&fog==result.Bindings["floor"].FogParamID&&fog!=noFog,"Source fog grouping/disabled row lost");
        Check(toneID==result.SceneBindings["floor2"].ToneMapID&&toneID!=result.SceneBindings["cave"].ToneMapID,"Tone source provenance rows conflated");
        foreach(var file in result.Files) {
            var before=GPARAM.Read(profiles[file.Path]);var g=GPARAM.Read(file.Bytes);
            var color=Value<Vector4>(g,"FogParamEditor","Fog-Color",fog);Near(color.X*color.X,95f/255*1.2f*.35f);Near(color.Y*color.Y,95f/255*1.2f*.35f);Near(color.Z*color.Z,92f/255*1.2f*.35f);Near(color.W,1);
            float strength=Value<float>(g,"FogParamEditor","Fog-fogAlpha",fog),distance=Value<float>(g,"FogParamEditor","Fog-Dist",fog),start=Value<float>(g,"FogParamEditor","Fog-DistOffset",fog);
            // Independent shader equation, not an assertion mirroring the writer.
            double Alpha(double metres)=>Math.Clamp(strength*(1-Math.Pow(2,-(metres-start)/distance)),0,1);
            Near((float)Alpha(10),0);Near((float)Alpha(25),0);Near((float)Alpha(50),.05842645f);Near((float)Alpha(100),.14335872f);
            Check(Alpha(1000)<=.300001&&Alpha(1000)>Alpha(100),"Calibrated horizon haze exceeds its controlled strength");
            Near(Value<float>(g,"FogParamEditor","Fog-fogContrast",fog),0);Check(Value<Vector4>(g,"FogParamEditor","Fog-heightColor",fog)==Vector4.Zero,"Unwanted native height fog remains enabled");Near(Value<float>(g,"FogParamEditor","Fog-fogAlpha",noFog),0);
            float originalExposure=Value<float>(before,"Tone Map[Yebis]","Yebis-ToneMapExposure",0),exposure=Value<float>(g,"Tone Map[Yebis]","Yebis-ToneMapExposure",toneID);
            Near(MathF.Log2(exposure/originalExposure),-.4f);Near(Value<float>(g,"Tone Map[Yebis]","Yebis-Gamma",toneID),2.2f);Near(Value<float>(g,"Tone Map[Yebis]","GS Presents - ToneMap-Exposure",toneID),0);
            foreach(var originalGroup in before.Params)foreach(var field in originalGroup.Fields)foreach(var value in field.Values) {
                var actual=g.Params.Single(p=>p.Key==originalGroup.Key).Fields[originalGroup.Fields.IndexOf(field)].Values.Single(v=>v.ID==value.ID);
                Check(Equals(value.Value,actual.Value)&&value.TimeOfDay==actual.TimeOfDay,"Fog/tone calibration changed an original native value");
            }
            foreach(var (key,id) in new[]{("FogParamEditor",fog),("Tone Map[Yebis]",toneID)})Check(g.UnkParamExtras.Single(e=>e.GroupIndex==g.Params.FindIndex(p=>p.Key==key)).Ids.Contains(id),"Calibrated fog/tone row missing runtime index");
        }
        Check(result.Rows.Single(r=>r.Kind=="Fog"&&r.SourceID==20).Status.Contains("Approximation"),"Source fog equivalence overclaimed");
        Near((float)result.Rows.Single(r=>r.Kind=="Tone"&&r.SourceID==20).Values["inverseToneMapMul"],3);
        var disabled=MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,[bindings[1]],"DS1R",calibration:new(false,0));Check(disabled.SceneBindings.Count==0&&disabled.Bindings["floor"].FogParamID==0,"Explicit native fog/tone mode still creates calibrated bindings");
        foreach(var invalid in new[]{calibration with{ToneEV=float.NaN},calibration with{ToneEV=9},calibration with{FogStrength=-1},calibration with{FogDistance=0},calibration with{FogStart=-1},calibration with{FogColorScale=float.PositiveInfinity}})Reject(()=>MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,bindings,"DS1R",calibration:invalid));
        var automatic=Profile();((GPARAM.IntField)automatic.Params.Single(p=>p.Key=="Tone Map[Yebis]").Fields.Single(f=>f.Key=="Yebis-AutoExposure")).Values[0].Value=1;
        Reject(()=>MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,new Dictionary<string,byte[]>{{"auto",automatic.Write()}},bindings,"DS1R"));
    }
    static void SourceColorCorrection() {
        var source=Source();var original=Profile().Write();var profiles=new Dictionary<string,byte[]>{{"phase",original}};
        var values=new Dictionary<string,float>{{"brightnessR",1},{"brightnessG",1},{"brightnessB",1},{"contrastR",1.15f},{"contrastG",1.15f},{"contrastB",1.15f},{"saturation",1.2f},{"hue",0}};
        var correct=Parameter("TONE_CORRECT_BANK",(20,values),(21,new(values){["saturation"]=.7f,["contrastG"]=.9f}));
        EnvironmentBinding[] bindings=[new("floor",20,20,20,255,true,20,20),new("annex",20,20,20,255,true,20,21),new("piece",20,20,20)];
        // Zero exposure adjustment still needs independent color-correction rows;
        // distinct correction IDs must not be conflated by a shared ToneMapID.
        var result=MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,bindings,"DS1R",calibration:new(false,0),toneCorrect:correct);
        var g=GPARAM.Read(result.Files.Single().Bytes);int floor=result.SceneBindings["floor"].ToneMapID,annex=result.SceneBindings["annex"].ToneMapID,grade=result.SceneBindings["floor"].ColorGradingID!.Value;
        Check(floor!=annex&&result.SceneBindings.Count==2&&!result.SceneBindings.ContainsKey("piece"),"Correction bindings lost their source pair or escaped collision scope");
        Near(Value<float>(g,"Tone Map[Yebis]","Yebis-ColorS",floor),1.2f);Near(Value<float>(g,"Tone Map[Yebis]","Yebis-Contrust",floor),1f);
        Near(Value<float>(g,"Tone Map[Yebis]","Yebis-ColorS",annex),.7f);Near(Value<float>(g,"Tone Map[Yebis]","Yebis-Contrust",annex),1f);
        Near(Value<float>(g,"Tone Map[Yebis]","Yebis-ToneMapExposure",floor),3f);Check(Value<int>(g,"Tone Map[Yebis]","Yebis-Chromatic Aberration Enable",floor)==0,"Native chromatic fringe remains enabled on imported correction");
        Check(Value<int>(g,"ColorGrading[Yebis]","Yebis-ColorGrading-Enable",grade)==0&&result.SceneBindings["annex"].ColorGradingID==grade,"Imported colors still inherit the native location LUT");
        foreach(var before in GPARAM.Read(original).Params)for(int n=0;n<before.Fields.Count;n++)foreach(var value in before.Fields[n].Values)
            Check(Equals(value.Value,g.Params.Single(p=>p.Key==before.Key).Fields[n].Values.Single(v=>v.ID==value.ID).Value),"Color correction mutated an original native row");
        foreach(var (key,id) in new[]{("Tone Map[Yebis]",floor),("Tone Map[Yebis]",annex),("ColorGrading[Yebis]",grade)})Check(g.UnkParamExtras.Single(e=>e.GroupIndex==g.Params.FindIndex(p=>p.Key==key)).Ids.Contains(id),"Imported correction row lacks a native runtime index");
        correct.Rows[0].Cells.Single(c=>c.Def.InternalName=="saturation").Value=float.NaN;
        Reject(()=>MapEnvironment.Convert(source.Light,source.Fog,source.Scatter,profiles,bindings,"DS1R",calibration:new(false,0),toneCorrect:correct));
    }
    internal static void Run() {
        LightEnergy();ShadowDirection();FogToneCalibration();SourceColorCorrection();
        var src=Source();var input=Profile().Write();var original=input.ToArray();var bindings=new[]{new EnvironmentBinding("ground",20,20,20),new EnvironmentBinding("foliage",23,255,255),new EnvironmentBinding("ground2",20,20,20)};
        var result=MapEnvironment.Convert(src.Light,src.Fog,src.Scatter,new Dictionary<string,byte[]>{{"phase0",input},{"phase1",input}},bindings,calibration:new(false,0));
        Check(result.Files.Count==2&&result.Bindings.Count==3,"Environment profiles or bindings omitted");
        Check(result.Bindings["ground"].LightSetID==result.Bindings["ground2"].LightSetID&&result.Bindings["ground"].LightSetID!=result.Bindings["foliage"].LightSetID,"Source light ID grouping was lost");
        Check(result.Bindings.Values.All(b=>b.LightSetID is not(30000 or 30001)),"Native field or index-only reserved row ID was reused");
        foreach(var file in result.Files) {
            var g=GPARAM.Read(file.Bytes);int light=result.Bindings["ground"].LightSetID,scatter=result.Bindings["ground"].LightScatteringID,noFog=result.Bindings["foliage"].FogParamID;
            var angle=Value<Vector2>(g,"LightSet ParamEditor","Directional Light Angle0",light);Near(angle.X,40*MathF.PI/180);Near(angle.Y,75*MathF.PI/180);
            Near(Value<Vector4>(g,"LightSet ParamEditor","Directional Light DiffColor0",light).W,1.5f);Near(Value<Vector4>(g,"LightSet ParamEditor","Directional Light Hemi Color Up",light).X,245f/255);Near(Value<Vector4>(g,"LightSet ParamEditor","Directional Light Hemi Color Up",light).W,.4f);
            Near(Value<Vector4>(g,"LightSet ParamEditor","Directional Light SpecColor0",light).W,0);Near(Value<Vector4>(g,"LightSet ParamEditor","Directional Light LocalLight SpecColor2",light).W,.5f);Near(Value<Vector2>(g,"LightSet ParamEditor","Directional Light LocalLight Angle2",light).Y,30*MathF.PI/180);
            var fields=g.Params.Single(p=>p.Key=="LightScattering : ParamEditor").Fields;Near((short)fields[3].Values.Single(v=>v.ID==scatter).Value,70);Near((short)fields[11].Values.Single(v=>v.ID==scatter).Value,205);Near((float)fields[7].Values.Single(v=>v.ID==scatter).Value,.35f);
            Near(Value<float>(g,"FogParamEditor","Fog-fogAlpha",noFog),0);Near(Value<float>(g,"FogParamEditor","Fog-heightFogDensity",noFog),0);Near(Value<float>(g,"FogParamEditor","Fog-Dist",0),42);
            Near(g.Unk40,1.25f);Check(g.Data30.SequenceEqual(new byte[]{7,9,13,17}),"Native opaque environment data changed");Near(Value<float>(g,"NativeUnrelated","sentinel",30000),123.25f);
            Check(g.Params.Single(p=>p.Key=="LightSet ParamEditor").Fields.All(f=>f.Values.Count==3),"Appended GPARAM capacities were not serialized");
            Check(g.UnkParamExtras.Single(e=>e.GroupIndex==0).Ids.Take(3).SequenceEqual(new[]{24120,30001,0}),"Native group-index IDs were removed or reordered");
            foreach(var row in result.Rows.Where(r=>r.NativeID!=null)) {
                string key=row.Kind switch{"Light"=>"LightSet ParamEditor","Scattering"=>"LightScattering : ParamEditor","Fog"=>"FogParamEditor",_=>throw new Exception("Unexpected row kind")};
                int group=g.Params.FindIndex(p=>p.Key==key);Check(g.UnkParamExtras.Single(e=>e.GroupIndex==group).Ids.Contains(row.NativeID!.Value),"Added native row is absent from the runtime group index");
            }
            Check(g.UnkParamExtras.Select(e=>e.GroupIndex).SequenceEqual(g.UnkParamExtras.Select(e=>e.GroupIndex).Order()),"New group indexes were appended out of native group order");
        }
        Check(result.Rows.Single(r=>r.Kind=="Fog"&&r.SourceID==20).NativeID==null&&result.Rows.Single(r=>r.Kind=="Fog"&&r.SourceID==20).Values.ContainsKey("fogBeginZ"),"Unsupported source fog was silently treated as converted");
        Check(input.SequenceEqual(original),"Native environment bytes changed");
        Reject(()=>MapEnvironment.Convert(src.Light,src.Fog,src.Scatter,new Dictionary<string,byte[]>{{"phase",input}},[bindings[0],bindings[0]]));
        var bad=Profile();bad.Params.Single(p=>p.Key=="LightScattering : ParamEditor").Fields.Reverse();Reject(()=>MapEnvironment.Convert(src.Light,src.Fog,src.Scatter,new Dictionary<string,byte[]>{{"phase",bad.Write()}},bindings));
        var duplicateIndex=Profile();duplicateIndex.UnkParamExtras.Add(duplicateIndex.UnkParamExtras[0].Clone());Reject(()=>MapEnvironment.Convert(src.Light,src.Fog,src.Scatter,new Dictionary<string,byte[]>{{"phase",duplicateIndex.Write()}},bindings));
        src.Light.Rows[0].Cells.Single(c=>c.Def.InternalName=="colR_0").Value=float.NaN;Reject(()=>MapEnvironment.Convert(src.Light,src.Fog,src.Scatter,new Dictionary<string,byte[]>{{"phase",input}},bindings));
        var skySource=Sky(false);var native=Sky(true);var nativeBytes=native.Write();var sky=MapEnvironment.BakeSky(skySource,native,p=>Definition(true,p.Contains("cloud")?.3f:7),_=>Definition(false,2),_=>"owned_sky.tif","Fixture");
        Check(sky.Model.Meshes.Count==2&&sky.Materials.Count==2,"Supported cloud/dome meshes were omitted or malformed landscape included");
        var reopened=FLVER2.Read(sky.Model.Write());Check(reopened.Meshes.Sum(m=>m.Vertices.Count)==6,"Sky FLVER did not round trip");Near(reopened.Meshes[0].Vertices[0].Colors[0].A,.25f);Near(reopened.Meshes[0].Vertices[0].Colors[0].R,.5f);
        Near((float)sky.Materials[0].Definition.Params.Single(p=>p.Name=="g_DiffuseColorPower").Value,.3f);Near((float)sky.Materials[1].Definition.Params.Single(p=>p.Name=="g_DiffuseColorPower").Value,7);
        Check(sky.Model.Materials.All(m=>m.Textures.Single().Path=="owned_sky.tif")&&sky.Materials.Select(m=>m.Name).Distinct().Count()==2,"Sky texture ownership or unique material names lost");
        Check(nativeBytes.SequenceEqual(native.Write())&&float.IsNaN(skySource.Meshes[0].Vertices[0].Position.X),"Sky assembly modified a source model");
        Console.WriteLine("PASS: binary-backed DS1R light energy and shadow angles, native row indexes, calibrated fog and relative Yebis exposure with original values preserved, separate DS1 behavior, typed scattering and source sky/cloud conversion");
    }
}
