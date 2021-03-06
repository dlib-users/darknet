#ifndef darknet_weights_visitor_h_INCLUDED
#define darknet_weights_visitor_h_INCLUDED

#include <cstring>
#include <dlib/dnn.h>

namespace darknet
{
    using namespace dlib;
    class weights_visitor
    {
        public:
        weights_visitor(const std::string& weights_path) : weights(read_bytes(weights_path))
        {
            int32_t major = 0, minor = 0, revision = 0;
            int32_t batches_seen1;
            int64_t batches_seen2;
            (*this) >> major >> minor >> revision;
            std::cout << "weights file '" << weights_path << "', major " << major << ", minor "
                      << minor << ", revision " << revision << ", batches seen ";

            if ((major * 10 + minor) >= 2 && major < 1000 && minor < 1000)
            {
                (*this) >> batches_seen2;
                std::cout << batches_seen2;
            }
            else
            {
                (*this) >> batches_seen1;
                std::cout << batches_seen1;
            }

            std::cout << ", num weights " << weights.size() << std::endl;
        }

        ~weights_visitor()
        {
            std::cout << "read " << offset << " floats of " << weights.size() << '\n';
        }

        // ignore other layers
        template <typename T> void operator()(size_t, T&) {}

        // batch normalization layers
        template <typename SUBNET> void operator()(size_t, add_layer<bn_<CONV_MODE>, SUBNET>& l)
        {
            auto& bn = l.layer_details();
            tensor& bn_t = bn.get_layer_params();
            auto bn_gamma = alias_tensor(1, l.subnet().get_output().k());
            auto bn_beta = alias_tensor(1, l.subnet().get_output().k());
            auto g = bn_gamma(bn_t, 0);
            auto b = bn_beta(bn_t, bn_gamma.size());
            DLIB_CASSERT(bn_t.size() == (g.size() + b.size()) && g.size() == b.size());

            const auto num_b = bn_t.size() / 2;

            // bn bias
            matrix<float> temp_b(1, num_b);
            for (size_t i = 0; i < num_b; ++i)
                (*this) >> temp_b(i);

            // bn weights
            matrix<float> temp_g(1, num_b);
            for (size_t i = 0; i < num_b; ++i)
                (*this) >> temp_g(i);

            // bn running mean
            matrix<float> temp_m(1, num_b);
            for (size_t i = 0; i < num_b; ++i)
                (*this) >> temp_m(i);

            // bn running var
            matrix<float> temp_v(1, num_b);
            for (size_t i = 0; i < num_b; ++i)
                (*this) >> temp_v(i);

            g = pointwise_divide(temp_g, sqrt(temp_v + DEFAULT_BATCH_NORM_EPS));
            b = temp_b - pointwise_multiply(mat(g), temp_m);

            // conv weight
            auto& conv = l.subnet().layer_details();
            auto& conv_t = conv.get_layer_params();
            DLIB_CASSERT(conv.bias_is_disabled());
            float* ptr = conv_t.host();
            for (size_t i = 0; i < conv_t.size(); ++i)
                (*this) >> ptr[i];
        }

        // convolutions
        template <long nf, long nr, long nc, int sy, int sx, int py, int px, typename SUBNET>
        void operator()(size_t, add_layer<con_<nf, nr, nc, sy, sx, py, px>, SUBNET>& l)
        {
            if (not l.layer_details().bias_is_disabled())
            {
                tensor& params = l.layer_details().get_layer_params();

                // Guess the number of input filters.
                // This is equivalent to calling: l.subnet().get_output().k().
                // However that requires the subnet to have a get_ouput() method,
                // which is not the case when the subnet is an input layer.
                // That would require a hack like tagging the input layer,
                // but it causes memory fluctuation and degrades performance.
                const long nf_in = (params.size() - nf) / nf / nr / nc;
                auto filters = alias_tensor(nf, nf_in, nr, nc);
                auto biases = alias_tensor(1, nf);
                auto f = filters(params, 0);
                auto b = biases(params, filters.size());
                DLIB_CASSERT(params.size() == (filters.size() + biases.size()));
                DLIB_CASSERT(f.size() == filters.size());
                DLIB_CASSERT(b.size() == biases.size());

                // conv bias
                float* ptr = b.host();
                for (size_t i = 0; i < b.size(); ++i)
                    (*this) >> ptr[i];

                // conv filters
                ptr = f.host();
                for (size_t i = 0; i < f.size(); ++i)
                    (*this) >> ptr[i];
            }
        }

        // fully connected layers
        template <unsigned long num_outputs, fc_bias_mode bias_mode, typename SUBNET>
        void operator()(size_t, add_layer<fc_<num_outputs, bias_mode>, SUBNET>& l)
        {
            // 1. fc bias
            // 2. fc weight
            const auto& sub_output = l.subnet().get_output();
            const int num_inputs = sub_output.nr() * sub_output.nc() * sub_output.k();

            auto& fc = l.layer_details();
            auto& params = fc.get_layer_params();
            auto filters_alias = alias_tensor(num_inputs, num_outputs);
            auto filters = filters_alias(params, 0);

            if (!fc.bias_is_disabled())
            {
                auto biases_alias = alias_tensor(1, num_outputs);
                auto biases = biases_alias(params, filters.size());

                // bias
                float* ptr = biases.host();
                for (size_t i = 0; i < biases.size(); i++)
                    (*this) >> ptr[i];
            }

            // weights - For some reason dlib's fc layer does not use the normal convention for
            // storing weights.  Usually the filters would have dimensions
            // [num_outputs,num_inputs], but dlib uses [num_inputs,num_outputs] So me must
            // transpose from darknet
            matrix<float> temp_f(num_outputs, num_inputs);
            for (long r = 0; r < temp_f.nr(); r++)  // num_outputs
            {
                for (long c = 0; c < temp_f.nc(); c++)  // num_inputs
                {
                    (*this) >> temp_f(r, c);
                }
            }

            // You don't need to do the following, instead you could modify the order of the
            // for-loops that follow. But this makes our intentions explicit.
            temp_f = trans(temp_f);

            float* ptr = filters.host();
            for (long r = 0; r < temp_f.nr(); ++r)  // num_inputs
            {
                for (long c = 0; c < temp_f.nc(); c++)  // num_outputs
                {
                    ptr[r * temp_f.nc() + c] = temp_f(r, c);
                }
            }
        }

        private:
        const std::vector<char> weights;
        size_t offset = 0;

        std::vector<char> read_bytes(const std::string& filename)
        {
            std::ifstream file(filename, std::ios::binary);
            file.seekg(0, std::ios::end);
            const size_t filesize = file.tellg();
            file.seekg(0);
            
            std::vector<char> v;
            v.reserve(filesize); //allocate the exact space required ONCE.
            
            std::copy(
                std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>(),
                std::back_inserter(v));
            return v;
        }

        template <typename T> weights_visitor& operator>>(T& x)
        {
            //Don't use type punning. 
            //Apparently, that's bad and can cause undefined behaviour. 
            //Instead, use memcpy()
            std::memcpy(&x, &weights[offset], sizeof(T));
            offset += sizeof(T);
            return *this;
        }
    };
}  // namespace darknet

#endif  // darknet_weights_visitor_h_INCLUDED
