#include <iostream>
#include <vector>
#include <chrono>
#include <array>
#include <memory>

#include <Eigen/Dense>

// ============================================================================================

template<typename T, int dim>
using Vect = Eigen::Array<T, dim, 1>;

template<typename T, int dim>
struct Particle {
  T mass;
  Vect<T, dim> pos, vel, acc;
};

template<int dim, typename T>
inline typename T::Scalar norm(const T &a, const T &b) {
  typename T::Scalar result = 0;
  for(int i = 0; i < dim; ++i)
    result += (a[i] - b[i])*(a[i] - b[i]);
  return std::sqrt(result);
}

template<typename T, int dim>
struct TreeNode {
  using P = Particle<T, dim>;
  using Vec = Vect<T, dim>;
  using ChildrenType = std::array<TreeNode, (1ull << dim)>;

  Eigen::Array<T, dim, 3> corners;
  ChildrenType *children = nullptr;
  std::vector<ChildrenType> *all_children = nullptr;

  P *particle = nullptr;
  T mass = 0.0;
  T h = -1.0;
  Vec mass_times_position = Vec::Zero();

  bool inside(const Vec &position) {
    for(int i = 0; i < dim; ++i)
      if(position[i] < corners(i, 0) || corners(i, 2) <= position[i]) {
        return false;
      }
    return true;
  }

  bool insert(const Vec &position, P *new_particle) {
    if(!inside(position)){
      return false;
    }
    if(particle) {
      if(!children) {
        children = create_children();
        for(size_t i = 0; i < children->size(); ++i)
          (*children)[i].insert(particle->pos, particle);
      }
      for(size_t i = 0; i < children->size(); ++i)
        (*children)[i].insert(new_particle->pos, new_particle);
    } else {
      particle = new_particle;
    }

    // update com
    mass += particle->mass;
    mass_times_position += particle->mass * particle->pos;
    return true;
  };

  ChildrenType *create_children() {
    all_children->push_back({});
    ChildrenType *new_children = &((*all_children).back());
    for(size_t i = 0; i < new_children->size(); ++i) {
      (*new_children)[i].all_children = all_children;
      for(int j = 0; j < dim; ++j) {
        const auto idx = (i >> j) % 2;
        (*new_children)[i].corners(j, 0) = corners(j, idx);
        (*new_children)[i].corners(j, 2) = corners(j, idx + 1);
        // middle
        (*new_children)[i].corners(j, 1) = (
            (*new_children)[i].corners(j, 0) + 
            (*new_children)[i].corners(j, 2)
          ) / 2.0;
      }
      (*new_children)[i].h = h / 2.0;
    }
    return new_children;
  }

  void traverse(const std::function<bool(const TreeNode<T, dim> &)> &f) const {
    if(f(*this))
      return;
    if(children) {
      for(auto &p : *children){
        p.traverse(f);
      }
    }
  }

  size_t nnodes() const {
    size_t nnodes_children = 0;
    if(children) {
      for(const auto &c : *children)
        nnodes_children += c.nnodes();
    }
    return 1 + nnodes_children;
  }
};

template<typename T, int dim>
class Tree {
  using P = Particle<T, dim>;
  using Vec = Vect<T, dim>;
  using ChildrenType = typename TreeNode<T, dim>::ChildrenType;

public:
  Tree(std::vector<P> &particles) : m_particles(particles), m_nodes(0) {
    m_nodes.reserve(m_particles.size() * (1ull << dim));

    for(int j = 0; j < dim; ++j) {
      m_tree.corners(j, 0) = std::numeric_limits<T>::max();
      m_tree.corners(j, 2) = -std::numeric_limits<T>::max();
    }
    for(const auto &p : m_particles) {
      for(int j = 0; j < dim; ++j) {
        constexpr T eps = 2.0 * std::numeric_limits<T>::epsilon();
        m_tree.corners(j, 0) = std::min(m_tree.corners(j, 0), p.pos[j]);
        m_tree.corners(j, 2) = std::max(m_tree.corners(j, 2), p.pos[j] + eps);
        m_tree.corners(j, 1) = (m_tree.corners(j, 0) + m_tree.corners(j, 2)) / 2.0;
      }
    }
    m_tree.h = norm<dim>(m_tree.corners.col(0), m_tree.corners.col(2));
    m_tree.all_children = &m_nodes;

    for(auto &p : m_particles) {
      m_tree.insert(p.pos, &p);
    }
  }

  std::vector<P> &get_particles() {
    return m_particles;
  }

  const TreeNode<T, dim> &get_root() const {
    return m_tree;
  }

  TreeNode<T, dim> &get_root() {
    return m_tree;
  }

  std::vector<ChildrenType> &get_nodes() {
    return m_nodes;
  }

  void set_nodes(const std::vector<ChildrenType> & nodes) {
    m_nodes = nodes;
  }

  void traverse_bfs(const std::function<bool(const TreeNode<T, dim> &)> &f) {
    std::vector<TreeNode<T, dim> *> curr = {&m_tree}, next;
    while(!curr.empty()) {
      for(auto &node : curr) {
        if(!f(*node)) {
          for(size_t i = 0; i < node->children->size(); ++i) {
            next.push_back(&(*node->children)[i]);
          }
        }
      }
      std::swap(curr, next);
      next.clear();
    }
  }

private:
  std::vector<P> &m_particles;
  std::vector<ChildrenType> m_nodes;
  TreeNode<T, dim> m_tree;
};

// ============================================================================================

constexpr double G = 1.0;
constexpr double softening = 1e-2;
constexpr double theta = 0.5;

template<typename T, int dim>
inline Vect<T, dim> compute_force(const Vect<T, dim> &a, const Vect<T, dim> &b) {
  const Vect<T, dim> r = a - b;
  const T inv_r3 = std::pow((r * r).sum() + softening, -1.5);
  return r * (G * inv_r3);
}

template<typename T, int dim>
void update_acceleration(std::vector<Particle<T, dim>> &particles) {
  for(auto &p : particles)
    for(size_t i = 0; i < dim; ++i)
      p.acc[i] = 0.0;
 
  for(size_t i = 0; i < particles.size(); ++i) {
    for(size_t j = 0; j < i; ++j) {
      const auto force = compute_force(particles[i].pos, particles[j].pos);
      particles[i].acc += force * particles[j].mass;
      particles[j].acc += force * particles[i].mass;
    }
  }
}

template<typename T, int dim>
void update_acceleration(Tree<T, dim> &tree) {
  auto &particles = tree.get_particles();
  for(auto &p : particles)
    for(size_t i = 0; i < dim; ++i)
      p.acc[i] = 0.0;
 
  for(size_t i = 0; i < particles.size(); ++i) {
    const auto f = [&](const TreeNode<T, dim> &node){
      //std::cout << &node << std::endl;
      if(node.children) {
        const Vect<T, dim> center_of_mass = node.mass_times_position / node.mass;
        const auto d = norm<dim>(center_of_mass, particles[i].pos);
        if(node.h / d < theta) {
          const auto force = compute_force(particles[i].pos, center_of_mass);
          particles[i].acc += force * node.mass;
          return true;
        } else {
          return false;
        }
      } else {
        if(&particles[i] == node.particle)
          return true;
        if(node.particle) {
          const auto force = compute_force(particles[i].pos, node.particle->pos);
          particles[i].acc += force * node.particle->mass;
        }
        return true;
      }
    };
    //tree.get_root().traverse(f);
    tree.traverse_bfs(f);
    //std::exit(-1);
  }
}

template<typename T, int dim>
void integrate(std::vector<Particle<T, dim>> &particles, double dt) {
  for(auto &p : particles) {
    p.pos += p.vel * dt;
    p.vel += p.acc * dt;
  }
}


template<typename T, int dim>
std::vector<Particle<T, dim>> initialize(size_t n) {
  using P = Particle<T, dim>;
  using Vec = Vect<T, dim>;

  T total_mass = 0.0;
  Vec total_momentum = Vec::Zero();
  std::vector<P> particles;
  for(size_t i = 0; i < n; ++i){
    particles.push_back({1.0, Vec::Random(), Vec::Random(), Vec::Zero()});
    total_mass += particles.back().mass;
    total_momentum += particles.back().vel * particles.back().mass;
  }
  
  const auto center_of_mass = total_momentum / total_mass;
  for(auto &p : particles)
    p.vel -= center_of_mass;
  return particles;
}


int main(int argc, char **argv) {
  using real = double;
  constexpr int dim = 3;

  if(argc < 2)
    return -1;

  double t_fin = 0.1;
  double dt = 1e-2;
  size_t n = std::atoll(argv[1]);

  using namespace std::chrono;

  {
    auto particles = initialize<real, dim>(n);
    const auto t0 = high_resolution_clock::now();
    for(double t = 0.0; t < t_fin; t += dt) {
      std::cout << "t:" << t << std::endl;
      update_acceleration<real, dim>(particles);
      integrate<real, dim>(particles, dt);
    }
    const auto t1 = high_resolution_clock::now();
    std::cout << "Elapsed: " << duration_cast<milliseconds>(t1 - t0).count() << std::endl;
  }

  {
    size_t t_tree = 0, t_update = 0, t_integration = 0;

    auto particles = initialize<real, dim>(n);
    
    for(double t = 0.0; t < t_fin; t += dt) {
      std::cout << "t:" << t << std::endl;

      auto t0 = high_resolution_clock::now();
      auto tree = Tree<real, dim>(particles);
      auto t1 = high_resolution_clock::now();
      t_tree += duration_cast<milliseconds>(t1 - t0).count();


      std::vector<TreeNode<real, dim> *> curr = {&tree.get_root()}, next;
      std::vector<typename TreeNode<real, dim>::ChildrenType *> nodes;
      while(!curr.empty()) {
        for(auto &node : curr) {
          if(node->children) {
            nodes.push_back(node->children);
            for(size_t i = 0; i < node->children->size(); ++i) {
              next.push_back(&(*node->children)[i]);
            }
          }
        }
        std::swap(curr, next);
        next.clear();
      }

      //std::cout << "nodes size" << nodes.size() << std::endl;


      //std::cout << "Start trick" << std::endl;
      std::vector<typename TreeNode<real, dim>::ChildrenType> tmp;
      std::vector<std::array<size_t, (1ull << dim)>> tmp2;
      for(auto np : nodes) {
        tmp.push_back(*np);
        tmp2.push_back({});

        for(size_t j = 0; j < (1ull << dim); ++j) {
          for(size_t i = 0; i < nodes.size(); ++i) {
            if( ((*np)[j]).children == nodes[i] ) {
              tmp2.back()[j] = i;
              //std::cout << "=" << i << std::endl;
              break;
            }
          }
        }
      }

      

      tree.set_nodes(tmp);

      for(size_t i = 0; i < nodes.size(); ++i) {
        for(size_t j = 0; j < (1ull << dim); ++j)
        if(tmp2[i][j] != 0)
          tree.get_nodes()[i][j].children = &tree.get_nodes()[tmp2[i][j]];
        else 
          tree.get_nodes()[i][j].children = nullptr;
      }
      //std::cout << "End trick" << std::endl;


      t0 = high_resolution_clock::now();
      update_acceleration<real, dim>(tree);
      t1 = high_resolution_clock::now();
      t_update += duration_cast<milliseconds>(t1 - t0).count();
      //std::cout << "nnodes" << tree.get_root().nnodes() << std::endl;

      t0 = high_resolution_clock::now();
      integrate<real, dim>(particles, dt);
      t1 = high_resolution_clock::now();
      t_integration += duration_cast<milliseconds>(t1 - t0).count();
    }
    
    std::cout << "Elapsed: " << t_tree + t_update + t_integration << std::endl;
    std::cout << " - tree: " << t_tree << std::endl;
    std::cout << " - update: " << t_update << std::endl;
    std::cout << " - integration: " << t_integration << std::endl;
  }


  return 0;
}
