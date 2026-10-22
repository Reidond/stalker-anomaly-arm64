-- Render scale: the finished scene (at the scene's size) into the back buffer, filtered up
function normal(shader, t_base, t_second, t_detail)
	shader:begin("stub_notransform_t","upscale_present")
			: fog	(false)
			: zb 	(false,false)
	shader:dx10texture	("s_upscale_in", "$user$scene_out")
	shader:dx10sampler	("smp_rtlinear")
end
