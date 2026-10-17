# A software ray marcher: bin/plugins/renderer_raymarch.so. Each mesh becomes a signed
# distance field with a sparse voxel octree over it, and every pixel is sphere traced on
# the CPU, then shown through SDL. A skeleton to build a custom renderer on; it needs no
# GPU features beyond showing a picture. player --renderer raymarch.
shockwave_plugin(renderer raymarch DEPS Shockwave::Renderer)
