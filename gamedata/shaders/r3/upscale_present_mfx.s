-- Render scale: the scene after MetalFX (at the output's size) into the back buffer
function normal(shader, t_base, t_second, t_detail)
	shader:begin("stub_notransform_t","upscale_present")
			: fog	(false)
			: zb 	(false,false)
	shader:dx10texture	("s_upscale_in", "$user$upscaled")
	shader:dx10sampler	("smp_rtlinear")
end
