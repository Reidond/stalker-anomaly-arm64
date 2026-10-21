// r4_shader_macros.inl: the D3D_SHADER_MACRO list and the shader-cache id that CRender::shader_compile hands to
// D3DCompile, plus the shader model chosen for the feature level. Textually included by r4.cpp and by
// tools/r4_shader_warmup, which runs it with the renderer's default settings.
//
// Expects, in the including scope: LPCSTR name, LPCSTR pTarget (reassigned here), CRender-style `o`, `m_skinning`,
// `m_ShaderOptions`, `HW`, `RImplementation` and the ps_* console variables. Produces `defines`, `def_it` and `sh_name`.
	D3D_SHADER_MACRO defines [128];
	int def_it = 0;
	char c_smapsize [32];
	char c_sun_shafts [32];
	char c_ssao [32];
	char c_sun_quality [32];
	char c_smaa_quality [32];
	
	// SSS preprocessor stuff
	char c_ssfx_sss_dir_quality[32];
	char c_ssfx_sss_omni_quality[32];
	char c_ssfx_terrain_pom_refine[32];
	char c_ssfx_pom_refine[32];
	char c_ssfx_il[32];
	char c_ssfx_ao[32];
	char c_ssfx_water[32];
	char c_ssfx_water_parallax[32];
	char c_ssr_quality[32];
	char c_rain_quality[32];
	char c_inter_grass[32];
	char c_shader_bus[32];

	char sh_name[MAX_PATH] = "";

	for (u32 i = 0; i < m_ShaderOptions.size(); ++i)
	{
		defines[def_it++] = m_ShaderOptions[i];
	}

	u32 len = xr_strlen(sh_name);
	// options
	{
		xr_sprintf(c_smapsize, "%04d", u32(o.smapsize));
		defines[def_it].Name = "SMAP_size";
		defines[def_it].Definition = c_smapsize;
		def_it ++;
		VERIFY(xr_strlen(c_smapsize) == 4);
		xr_strcat(sh_name, c_smapsize);
		len += 4;
	}

	if (o.fp16_filter)
	{
		defines[def_it].Name = "FP16_FILTER";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.fp16_filter);
	++len;

	if (o.fp16_blend)
	{
		defines[def_it].Name = "FP16_BLEND";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.fp16_blend);
	++len;

	if (o.HW_smap)
	{
		defines[def_it].Name = "USE_HWSMAP";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.HW_smap);
	++len;

	if (o.HW_smap_PCF)
	{
		defines[def_it].Name = "USE_HWSMAP_PCF";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.HW_smap_PCF);
	++len;

	if (o.HW_smap_FETCH4)
	{
		defines[def_it].Name = "USE_FETCH4";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.HW_smap_FETCH4);
	++len;

	if (o.sjitter)
	{
		defines[def_it].Name = "USE_SJITTER";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.sjitter);
	++len;

	if (HW.Caps.raster_major >= 3)
	{
		defines[def_it].Name = "USE_BRANCHING";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(HW.Caps.raster_major >= 3);
	++len;

	if (HW.Caps.geometry.bVTF)
	{
		defines[def_it].Name = "USE_VTF";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(HW.Caps.geometry.bVTF);
	++len;

	if (o.Tshadows)
	{
		defines[def_it].Name = "USE_TSHADOWS";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.Tshadows);
	++len;

	if (ps_r2_anomaly_flags.test(R2_AN_FLAG_MBLUR))
	{
		defines[def_it].Name = "USE_MBLUR";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(ps_r2_anomaly_flags.test(R2_AN_FLAG_MBLUR));
	++len;

	if (o.sunfilter)
	{
		defines[def_it].Name = "USE_SUNFILTER";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.sunfilter);
	++len;

	if (o.sunstatic)
	{
		defines[def_it].Name = "USE_R2_STATIC_SUN";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.sunstatic);
	++len;

	if (o.forceskinw)
	{
		defines[def_it].Name = "SKIN_COLOR";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.forceskinw);
	++len;

	if (o.ssao_blur_on)
	{
		defines[def_it].Name = "USE_SSAO_BLUR";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.ssao_blur_on);
	++len;

	if (o.ssao_hdao)
	{
		defines[def_it].Name = "HDAO";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0' + char(o.ssao_hbao);
		++len;
		sh_name[len] = '0' + char(o.ssao_half_data);
		++len;
		if (o.ssao_hbao)
		{
			defines[def_it].Name = "SSAO_OPT_DATA";
			if (o.ssao_half_data)
			{
				defines[def_it].Definition = "2";
			}
			else
			{
				defines[def_it].Definition = "1";
			}
			def_it ++;

			if (o.hbao_vectorized)
			{
				defines[def_it].Name = "VECTORIZED_CODE";
				defines[def_it].Definition = "1";
				def_it ++;
			}

			defines[def_it].Name = "USE_HBAO";
			defines[def_it].Definition = "1";
			def_it ++;
		}
	}

	if (o.dx10_msaa)
	{
		static char def[ 256 ];
		//if( m_MSAASample < 0 )
		//{
		def[0] = '0';
		//	sh_name[len]='0'; ++len;
		//}
		//else
		//{
		//	def[0]= '0' + char(m_MSAASample);
		//	sh_name[len]='0' + char(m_MSAASample); ++len;
		//}
		def[1] = 0;
		defines[def_it].Name = "ISAMPLE";
		defines[def_it].Definition = def;
		def_it ++;
		sh_name[len] = '0';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	// skinning
	if (m_skinning < 0)
	{
		defines[def_it].Name = "SKIN_NONE";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (0 == m_skinning)
	{
		defines[def_it].Name = "SKIN_0";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(0 == m_skinning);
	++len;

	if (1 == m_skinning)
	{
		defines[def_it].Name = "SKIN_1";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(1 == m_skinning);
	++len;

	if (2 == m_skinning)
	{
		defines[def_it].Name = "SKIN_2";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(2 == m_skinning);
	++len;

	if (3 == m_skinning)
	{
		defines[def_it].Name = "SKIN_3";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(3 == m_skinning);
	++len;

	if (4 == m_skinning)
	{
		defines[def_it].Name = "SKIN_4";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(4 == m_skinning);
	++len;

	//	Igor: need restart options
	if (RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_SOFT_WATER))
	{
		defines[def_it].Name = "USE_SOFT_WATER";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_SOFT_PARTICLES))
	{
		defines[def_it].Name = "USE_SOFT_PARTICLES";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_DOF))
	{
		defines[def_it].Name = "USE_DOF";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_sunshafts_mode)
	{
		xr_sprintf(c_sun_shafts, "%d", ps_r_sun_shafts);
		defines[def_it].Name = "SUN_SHAFTS_QUALITY";
		defines[def_it].Definition = c_sun_shafts;
		def_it ++;
		sh_name[len] = '0' + char(ps_r_sun_shafts);
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_r_ssao)
	{
		xr_sprintf(c_ssao, "%d", ps_r_ssao);
		defines[def_it].Name = "SSAO_QUALITY";
		defines[def_it].Definition = c_ssao;
		def_it ++;
		sh_name[len] = '0' + char(ps_r_ssao);
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_r_sun_quality)
	{
		xr_sprintf(c_sun_quality, "%d", ps_r_sun_quality);
		defines[def_it].Name = "SUN_QUALITY";
		defines[def_it].Definition = c_sun_quality;
		def_it ++;
		sh_name[len] = '0' + char(ps_r_sun_quality);
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_STEEP_PARALLAX))
	{
		defines[def_it].Name = "ALLOW_STEEPPARALLAX";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	//////////////////////////////////lvutner	

	if (ps_r2_anomaly_flags.test(R2_AN_FLAG_WATER_REFLECTIONS))
	{
		defines[def_it].Name = "USE_REFLECTIONS";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (ps_smaa_quality)
	{
		xr_sprintf(c_smaa_quality, "%d", ps_smaa_quality);
		defines[def_it].Name = "SMAA_QUALITY";
		defines[def_it].Definition = c_smaa_quality;
		def_it++;
		sh_name[len] = '0' + char(ps_smaa_quality);
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	//Useful shit. 
	if (HW.Caps.id_vendor == 0x1002) //AMD hardware
	{
		defines[def_it].Name = "INT_RENDER_AMD";
		defines[def_it].Definition = "1";
		def_it++;
	}

	if (HW.Caps.id_vendor == 0x10DE) //NVidia hardware
	{
		defines[def_it].Name = "INT_RENDER_NVIDIA";
		defines[def_it].Definition = "1";
		def_it++;
	}
	
	/////////////////////////////////////////////


	//R_ASSERT						( !o.dx10_sm4_1 );
	if (o.dx10_sm4_1)
	{
		defines[def_it].Name = "SM_4_1";
		defines[def_it].Definition = "1";
		def_it++;
	}
	sh_name[len] = '0' + char(o.dx10_sm4_1);
	++len;

	R_ASSERT(HW.FeatureLevel>=D3D_FEATURE_LEVEL_11_0);
	if (HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0)
	{
		defines[def_it].Name = "SM_5";
		defines[def_it].Definition = "1";
		def_it++;
	}
	sh_name[len] = '0' + char(HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0);
	++len;

	if (o.dx10_minmax_sm)
	{
		defines[def_it].Name = "USE_MINMAX_SM";
		defines[def_it].Definition = "1";
		def_it++;
	}
	sh_name[len] = '0' + char(o.dx10_minmax_sm != 0);
	++len;
	
	if (ps_ssfx_rain_1.w > 0)
	{
		xr_sprintf(c_rain_quality, "%d", u8(ps_ssfx_rain_1.w));
		defines[def_it].Name = "SSFX_RAIN_QUALITY";
		defines[def_it].Definition = c_rain_quality;
		def_it++;
		xr_strcat(sh_name, c_rain_quality);
		len += xr_strlen(c_rain_quality);
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (ps_ssfx_grass_interactive.y > 0)
	{
		xr_sprintf(c_inter_grass, "%d", u8(ps_ssfx_grass_interactive.y));
		defines[def_it].Name = "SSFX_INT_GRASS";
		defines[def_it].Definition = c_inter_grass;
		def_it++;
		xr_strcat(sh_name, c_inter_grass);
		len += xr_strlen(c_inter_grass);
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	xr_sprintf(c_ssr_quality, "%d", u8(std::min(std::max(ps_ssfx_ssr_quality, 0), 5)));
	defines[def_it].Name = "SSFX_SSR_QUALITY";
	defines[def_it].Definition = c_ssr_quality;
	def_it++;
	xr_strcat(sh_name, c_ssr_quality);
	len += xr_strlen(c_ssr_quality);

	xr_sprintf(c_ssfx_water, "%d", u8(std::min(std::max(ps_ssfx_water_quality.x, 0.0f), 4.0f)));
	defines[def_it].Name = "SSFX_WATER_QUALITY";
	defines[def_it].Definition = c_ssfx_water;
	def_it++;
	xr_strcat(sh_name, c_ssfx_water);
	len += xr_strlen(c_ssfx_water);

	xr_sprintf(c_ssfx_water_parallax, "%d", u8(std::min(std::max(ps_ssfx_water_quality.y, 0.0f), 3.0f)));
	defines[def_it].Name = "SSFX_WATER_PARALLAX";
	defines[def_it].Definition = c_ssfx_water_parallax;
	def_it++;
	xr_strcat(sh_name, c_ssfx_water_parallax);
	len += xr_strlen(c_ssfx_water_parallax);

	xr_sprintf(c_ssfx_il, "%d", u8(std::min(std::max(ps_ssfx_il_quality, 0), 64)));
	defines[def_it].Name = "SSFX_IL_QUALITY";
	defines[def_it].Definition = c_ssfx_il;
	def_it++;
	xr_strcat(sh_name, c_ssfx_il);
	len += xr_strlen(c_ssfx_il);

	xr_sprintf(c_ssfx_ao, "%d", u8(std::min(std::max(ps_ssfx_ao_quality, 2), 8)));
	defines[def_it].Name = "SSFX_AO_QUALITY";
	defines[def_it].Definition = c_ssfx_ao;
	def_it++;
	xr_strcat(sh_name, c_ssfx_ao);
	len += xr_strlen(c_ssfx_ao);

	xr_sprintf(c_ssfx_pom_refine, "%d", u8(std::min(std::max(ps_ssfx_pom_refine, 0), 1)));
	defines[def_it].Name = "SSFX_POM_REFINE";
	defines[def_it].Definition = c_ssfx_pom_refine;
	def_it++;
	xr_strcat(sh_name, c_ssfx_pom_refine);
	len += xr_strlen(c_ssfx_pom_refine);

	xr_sprintf(c_ssfx_terrain_pom_refine, "%d", u8(std::min(std::max(ps_ssfx_terrain_pom_refine, 0), 1)));
	defines[def_it].Name = "SSFX_TERRA_POM_REFINE";
	defines[def_it].Definition = c_ssfx_terrain_pom_refine;
	def_it++;
	xr_strcat(sh_name, c_ssfx_terrain_pom_refine);
	len += xr_strlen(c_ssfx_terrain_pom_refine);

	xr_sprintf(c_ssfx_sss_dir_quality, "%d", u8(std::min(std::max((int)ps_ssfx_sss_quality.x, 1), 24)));
	defines[def_it].Name = "SSFX_SSS_DIR_QUALITY";
	defines[def_it].Definition = c_ssfx_sss_dir_quality;
	def_it++;
	xr_strcat(sh_name, c_ssfx_sss_dir_quality);
	len += xr_strlen(c_ssfx_sss_dir_quality);

	xr_sprintf(c_ssfx_sss_omni_quality, "%d", u8(std::min(std::max((int)ps_ssfx_sss_quality.y, 1), 12)));
	defines[def_it].Name = "SSFX_SSS_OMNI_QUALITY";
	defines[def_it].Definition = c_ssfx_sss_omni_quality;
	def_it++;
	xr_strcat(sh_name, c_ssfx_sss_omni_quality);
	len += xr_strlen(c_ssfx_sss_omni_quality);

	defines[def_it].Name = "SSFX_MODEXE";
	defines[def_it].Definition = "1";
	def_it++;
	sh_name[len] = '1';
	++len;

	//Be carefull!!!!! this should be at the end to correctly generate
	//compiled shader name;
	// add a #define for DX10_1 MSAA support
	if (o.dx10_msaa)
	{
		defines[def_it].Name = "USE_MSAA";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;

		static char samples[2];

		defines[def_it].Name = "MSAA_SAMPLES";
		samples[0] = char(o.dx10_msaa_samples) + '0';
		samples[1] = 0;
		defines[def_it].Definition = samples;
		def_it ++;
		sh_name[len] = '0' + char(o.dx10_msaa_samples);
		++len;

		if (o.dx10_msaa_opt)
		{
			defines[def_it].Name = "MSAA_OPTIMIZATION";
			defines[def_it].Definition = "1";
			def_it ++;
		}
		sh_name[len] = '0' + char(o.dx10_msaa_opt);
		++len;

		switch (o.dx10_msaa_alphatest)
		{
		case MSAA_ATEST_DX10_0_ATOC:
			defines[def_it].Name = "MSAA_ALPHATEST_DX10_0_ATOC";
			defines[def_it].Definition = "1";
			def_it ++;
			sh_name[len] = '1';
			++len;
			sh_name[len] = '0';
			++len;
			sh_name[len] = '0';
			++len;
			break;
		case MSAA_ATEST_DX10_1_ATOC:
			defines[def_it].Name = "MSAA_ALPHATEST_DX10_1_ATOC";
			defines[def_it].Definition = "1";
			def_it ++;
			sh_name[len] = '0';
			++len;
			sh_name[len] = '1';
			++len;
			sh_name[len] = '0';
			++len;
			break;
		case MSAA_ATEST_DX10_1_NATIVE:
			defines[def_it].Name = "MSAA_ALPHATEST_DX10_1";
			defines[def_it].Definition = "1";
			def_it ++;
			sh_name[len] = '0';
			++len;
			sh_name[len] = '0';
			++len;
			sh_name[len] = '1';
			++len;
			break;
		default:
			sh_name[len] = '0';
			++len;
			sh_name[len] = '0';
			++len;
			sh_name[len] = '0';
			++len;
		}
	}
	else
	{
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
	}

	// the bus version, in the cache name too so each version keeps its own build
	xr_sprintf(c_shader_bus, "%d", ShaderBus::version());
	defines[def_it].Name = "SHADER_BUS";
	defines[def_it].Definition = c_shader_bus;
	def_it++;
	xr_strcat(sh_name, c_shader_bus);
	len += xr_strlen(c_shader_bus);

	sh_name[len] = 0;

	// finish
	defines[def_it].Name = 0;
	defines[def_it].Definition = 0;
	def_it ++;

	// 
	if (0 == xr_strcmp(pFunctionName, "main"))
	{
		if ('v' == pTarget[0])
		{
			if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_0)
				pTarget = "vs_4_0";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_1)
				pTarget = "vs_4_1";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_11_0)
				pTarget = "vs_5_0";
		}
		else if ('p' == pTarget[0])
		{
			if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_0)
				pTarget = "ps_4_0";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_1)
				pTarget = "ps_4_1";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_11_0)
				pTarget = "ps_5_0";
		}
		else if ('g' == pTarget[0])
		{
			if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_0)
				pTarget = "gs_4_0";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_1)
				pTarget = "gs_4_1";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_11_0)
				pTarget = "gs_5_0";
		}
		else if ('c' == pTarget[0])
		{
			if (HW.FeatureLevel == D3D_FEATURE_LEVEL_11_0)
				pTarget = "cs_5_0";
		}
	}
